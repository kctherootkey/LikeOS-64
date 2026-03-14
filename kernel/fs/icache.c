// LikeOS-64 Inode Cache
//
// Caches per-file metadata indexed by start_cluster.
// Reference counted: open handles hold refs, inodes stay cached after close.
// Per-inode I/O locks enable concurrent cached reads without the global
// fat32_io_lock.
//
// Hash table with per-bucket spinlocks.  LRU list for evicting
// zero-refcount inodes when the cache is full.

#include "../../include/kernel/icache.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"

// ============================================================================
// Hash table bucket
// ============================================================================

typedef struct {
    ic_inode_t*  head;
    spinlock_t   lock;
} ic_bucket_t;

static ic_bucket_t ic_hash[IC_HASH_BUCKETS];

// ============================================================================
// Global LRU list (doubly-linked circular with sentinel)
// Only zero-refcount inodes are on the LRU list.
// ============================================================================

static ic_inode_t  ic_lru_sentinel;
static spinlock_t  ic_lru_lock;
static volatile uint64_t ic_entry_count;

// ============================================================================
// Statistics
// ============================================================================

static volatile uint64_t ic_stat_hits;
static volatile uint64_t ic_stat_misses;
static volatile uint64_t ic_stat_evictions;

// ============================================================================
// Initialization
// ============================================================================

static int ic_initialized = 0;

void icache_init(void)
{
    for (int i = 0; i < IC_HASH_BUCKETS; i++) {
        ic_hash[i].head = 0;
        spinlock_init(&ic_hash[i].lock, "icache");
    }
    ic_lru_sentinel.lru_prev = &ic_lru_sentinel;
    ic_lru_sentinel.lru_next = &ic_lru_sentinel;
    spinlock_init(&ic_lru_lock, "ic_lru");
    ic_entry_count  = 0;
    ic_stat_hits    = 0;
    ic_stat_misses  = 0;
    ic_stat_evictions = 0;
    ic_initialized  = 1;
}

// ============================================================================
// Hash function
// ============================================================================

static inline unsigned long ic_bucket_index(unsigned long start_cluster)
{
    return (start_cluster * 2654435761UL) & IC_HASH_MASK;
}

// ============================================================================
// LRU helpers (caller must hold ic_lru_lock)
// ============================================================================

static void ic_lru_add(ic_inode_t *n)
{
    n->lru_next = ic_lru_sentinel.lru_next;
    n->lru_prev = &ic_lru_sentinel;
    ic_lru_sentinel.lru_next->lru_prev = n;
    ic_lru_sentinel.lru_next = n;
}

static void ic_lru_remove(ic_inode_t *n)
{
    if (n->lru_prev)
        n->lru_prev->lru_next = n->lru_next;
    if (n->lru_next)
        n->lru_next->lru_prev = n->lru_prev;
    n->lru_prev = 0;
    n->lru_next = 0;
}

// ============================================================================
// Eviction — evict one zero-refcount inode from LRU tail
// ============================================================================

static void ic_evict_one(void)
{
    uint64_t lru_flags;
    spin_lock_irqsave(&ic_lru_lock, &lru_flags);

    ic_inode_t *victim = ic_lru_sentinel.lru_prev;
    if (victim == &ic_lru_sentinel) {
        spin_unlock_irqrestore(&ic_lru_lock, lru_flags);
        return; // empty
    }
    // Only evict zero-refcount inodes
    if (victim->refcount > 0) {
        // Walk backwards to find a zero-refcount candidate
        while (victim != &ic_lru_sentinel && victim->refcount > 0)
            victim = victim->lru_prev;
        if (victim == &ic_lru_sentinel) {
            spin_unlock_irqrestore(&ic_lru_lock, lru_flags);
            return; // all inodes are in use, can't evict
        }
    }
    ic_lru_remove(victim);
    spin_unlock_irqrestore(&ic_lru_lock, lru_flags);

    // Remove from hash bucket
    unsigned long bucket = ic_bucket_index(victim->start_cluster);
    uint64_t bucket_flags;
    spin_lock_irqsave(&ic_hash[bucket].lock, &bucket_flags);
    ic_inode_t **pp = &ic_hash[bucket].head;
    while (*pp) {
        if (*pp == victim) {
            *pp = victim->hash_next;
            break;
        }
        pp = &(*pp)->hash_next;
    }
    spin_unlock_irqrestore(&ic_hash[bucket].lock, bucket_flags);

    if (victim->chain)
        kfree(victim->chain);
    kfree(victim);
    __sync_fetch_and_sub(&ic_entry_count, 1);
    __sync_fetch_and_add(&ic_stat_evictions, 1);
}

// ============================================================================
// Lookup
// ============================================================================

ic_inode_t* icache_lookup(unsigned long start_cluster)
{
    if (!ic_initialized || start_cluster < 2)
        return 0;

    unsigned long bucket = ic_bucket_index(start_cluster);
    uint64_t flags;
    spin_lock_irqsave(&ic_hash[bucket].lock, &flags);

    ic_inode_t *n = ic_hash[bucket].head;
    while (n) {
        if (n->start_cluster == start_cluster) {
            spin_unlock_irqrestore(&ic_hash[bucket].lock, flags);
            __sync_fetch_and_add(&ic_stat_hits, 1);
            return n;
        }
        n = n->hash_next;
    }
    spin_unlock_irqrestore(&ic_hash[bucket].lock, flags);
    __sync_fetch_and_add(&ic_stat_misses, 1);
    return 0;
}

// ============================================================================
// Get or create
// ============================================================================

ic_inode_t* icache_get(unsigned long start_cluster, unsigned long size,
                       unsigned int attr, unsigned long parent_cluster,
                       unsigned long dirent_cluster, unsigned int dirent_index,
                       uint16_t wrt_time, uint16_t wrt_date)
{
    if (!ic_initialized || start_cluster < 2)
        return 0;

    // Try lookup first
    unsigned long bucket = ic_bucket_index(start_cluster);
    uint64_t flags;
    spin_lock_irqsave(&ic_hash[bucket].lock, &flags);

    ic_inode_t *n = ic_hash[bucket].head;
    while (n) {
        if (n->start_cluster == start_cluster) {
            // Found — update metadata and bump refcount
            n->size = size;
            n->attr = attr;
            n->parent_cluster = parent_cluster;
            n->dirent_cluster = dirent_cluster;
            n->dirent_index = dirent_index;
            n->wrt_time = wrt_time;
            n->wrt_date = wrt_date;
            n->flags |= IC_VALID;
            __sync_fetch_and_add(&n->refcount, 1);
            // Remove from LRU if it was there (refcount was 0, now > 0)
            if (n->lru_prev || n->lru_next) {
                uint64_t lru_flags;
                spin_lock_irqsave(&ic_lru_lock, &lru_flags);
                ic_lru_remove(n);
                spin_unlock_irqrestore(&ic_lru_lock, lru_flags);
            }
            spin_unlock_irqrestore(&ic_hash[bucket].lock, flags);
            __sync_fetch_and_add(&ic_stat_hits, 1);
            return n;
        }
        n = n->hash_next;
    }
    spin_unlock_irqrestore(&ic_hash[bucket].lock, flags);

    // Not found — allocate new inode
    while (ic_entry_count >= IC_MAX_ENTRIES)
        ic_evict_one();

    ic_inode_t *inode = (ic_inode_t *)kalloc(sizeof(ic_inode_t));
    if (!inode)
        return 0;
    mm_memset(inode, 0, sizeof(ic_inode_t));
    inode->start_cluster = start_cluster;
    inode->size = size;
    inode->attr = attr;
    inode->parent_cluster = parent_cluster;
    inode->dirent_cluster = dirent_cluster;
    inode->dirent_index = dirent_index;
    inode->wrt_time = wrt_time;
    inode->wrt_date = wrt_date;
    inode->refcount = 1;
    inode->flags = IC_VALID;
    inode->io_locked = 0;
    spinlock_init(&inode->io_wait_lock, "inode_io");
    inode->chain     = 0;
    inode->chain_len = 0;
    inode->chain_cap = 0;

    // Insert into hash bucket
    spin_lock_irqsave(&ic_hash[bucket].lock, &flags);
    inode->hash_next = ic_hash[bucket].head;
    ic_hash[bucket].head = inode;
    spin_unlock_irqrestore(&ic_hash[bucket].lock, flags);

    __sync_fetch_and_add(&ic_entry_count, 1);
    __sync_fetch_and_add(&ic_stat_misses, 1);
    return inode;
}

// ============================================================================
// Reference counting
// ============================================================================

void icache_ref(ic_inode_t *inode)
{
    if (!inode)
        return;
    int old = __sync_fetch_and_add(&inode->refcount, 1);
    // If was on LRU (refcount was 0), remove it
    if (old == 0 && (inode->lru_prev || inode->lru_next)) {
        uint64_t flags;
        spin_lock_irqsave(&ic_lru_lock, &flags);
        ic_lru_remove(inode);
        spin_unlock_irqrestore(&ic_lru_lock, flags);
    }
}

void icache_unref(ic_inode_t *inode)
{
    if (!inode)
        return;
    int new_rc = __sync_sub_and_fetch(&inode->refcount, 1);
    if (new_rc <= 0) {
        // Refcount reached zero — add to LRU for possible eviction
        if (new_rc < 0)
            inode->refcount = 0; // clamp
        uint64_t flags;
        spin_lock_irqsave(&ic_lru_lock, &flags);
        ic_lru_add(inode);
        spin_unlock_irqrestore(&ic_lru_lock, flags);
    }
}

// ============================================================================
// Metadata update
// ============================================================================

void icache_update_size(ic_inode_t *inode, unsigned long new_size)
{
    if (!inode)
        return;
    inode->size = new_size;
    inode->flags |= IC_DIRTY;
}

void icache_mark_dirty(ic_inode_t *inode)
{
    if (!inode)
        return;
    inode->flags |= IC_DIRTY;
}

// ============================================================================
// Flush dirty inode metadata to disk
// Write back the dirent entry in the parent directory.
// ============================================================================

int icache_flush(ic_inode_t *inode)
{
    if (!inode || !(inode->flags & IC_DIRTY))
        return 0;
    if (inode->dirent_cluster < 2)
        return 0;

    extern fat32_fs_t *g_root_fs;
    if (!g_root_fs)
        return -1;

    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void *buf = kalloc(cluster_size);
    if (!buf)
        return -1;

    fat32_io_lock();

    // Read the directory cluster containing this inode's dirent
    unsigned long lba = g_root_fs->part_lba_offset + g_root_fs->data_start_lba +
                        (inode->dirent_cluster - 2) * g_root_fs->sectors_per_cluster;
    if (g_root_fs->bdev->read((block_device_t *)g_root_fs->bdev, lba,
                               g_root_fs->sectors_per_cluster, buf) != 0) {
        fat32_io_unlock();
        kfree(buf);
        return -1;
    }

    // Update the dirent
    typedef struct {
        uint8_t  name[11];
        uint8_t  attr;
        uint8_t  ntRes;
        uint8_t  crtTimeTenth;
        uint16_t crtTime;
        uint16_t crtDate;
        uint16_t lstAccDate;
        uint16_t fstClusHI;
        uint16_t wrtTime;
        uint16_t wrtDate;
        uint16_t fstClusLO;
        uint32_t fileSize;
    } __attribute__((packed)) fat32_dirent_raw_t;

    fat32_dirent_raw_t *ents = (fat32_dirent_raw_t *)buf;
    unsigned max_entries = cluster_size / sizeof(fat32_dirent_raw_t);

    if (inode->dirent_index < max_entries) {
        fat32_dirent_raw_t *de = &ents[inode->dirent_index];
        de->fileSize = (uint32_t)inode->size;
        de->wrtTime = inode->wrt_time;
        de->wrtDate = inode->wrt_date;

        // Write back
        if (g_root_fs->bdev->write) {
            g_root_fs->bdev->write((block_device_t *)g_root_fs->bdev, lba,
                                    g_root_fs->sectors_per_cluster, buf);
        }
    }

    fat32_io_unlock();
    kfree(buf);
    inode->flags &= ~IC_DIRTY;
    return 0;
}

int icache_flush_all(void)
{
    if (!ic_initialized)
        return 0;

    int flushed = 0;
    for (int b = 0; b < IC_HASH_BUCKETS; b++) {
        uint64_t flags;
        spin_lock_irqsave(&ic_hash[b].lock, &flags);
        ic_inode_t *n = ic_hash[b].head;
        while (n) {
            if (n->flags & IC_DIRTY) {
                spin_unlock_irqrestore(&ic_hash[b].lock, flags);
                icache_flush(n);
                flushed++;
                spin_lock_irqsave(&ic_hash[b].lock, &flags);
                // Restart from head since list may have changed
                n = ic_hash[b].head;
                continue;
            }
            n = n->hash_next;
        }
        spin_unlock_irqrestore(&ic_hash[b].lock, flags);
    }
    return flushed;
}

// ============================================================================
// Removal (on unlink)
// ============================================================================

void icache_remove(unsigned long start_cluster)
{
    if (!ic_initialized || start_cluster < 2)
        return;

    unsigned long bucket = ic_bucket_index(start_cluster);
    uint64_t flags;
    spin_lock_irqsave(&ic_hash[bucket].lock, &flags);

    ic_inode_t **pp = &ic_hash[bucket].head;
    while (*pp) {
        ic_inode_t *n = *pp;
        if (n->start_cluster == start_cluster) {
            *pp = n->hash_next;
            spin_unlock_irqrestore(&ic_hash[bucket].lock, flags);

            uint64_t lru_flags;
            spin_lock_irqsave(&ic_lru_lock, &lru_flags);
            ic_lru_remove(n);
            spin_unlock_irqrestore(&ic_lru_lock, lru_flags);

            if (n->chain)
                kfree(n->chain);
            kfree(n);
            __sync_fetch_and_sub(&ic_entry_count, 1);
            return;
        }
        pp = &(*pp)->hash_next;
    }
    spin_unlock_irqrestore(&ic_hash[bucket].lock, flags);
}

void icache_invalidate_all(void)
{
    if (!ic_initialized)
        return;

    for (int b = 0; b < IC_HASH_BUCKETS; b++) {
        uint64_t flags;
        spin_lock_irqsave(&ic_hash[b].lock, &flags);

        ic_inode_t *n = ic_hash[b].head;
        while (n) {
            ic_inode_t *next = n->hash_next;
            if (n->chain)
                kfree(n->chain);
            kfree(n);
            n = next;
        }
        ic_hash[b].head = 0;
        spin_unlock_irqrestore(&ic_hash[b].lock, flags);
    }

    uint64_t lru_flags;
    spin_lock_irqsave(&ic_lru_lock, &lru_flags);
    ic_lru_sentinel.lru_prev = &ic_lru_sentinel;
    ic_lru_sentinel.lru_next = &ic_lru_sentinel;
    spin_unlock_irqrestore(&ic_lru_lock, lru_flags);

    ic_entry_count = 0;
}

// ============================================================================
// Cluster chain cache
// ============================================================================

// Initial and growth factor for chain array.
#define CC_INIT_CAP   64
#define CC_MAX_CAP    (1024 * 1024)   // 1M entries ~ 8MB (generous)

// Extend chain_map from chain_len up to (and including) index `target_idx`.
// Caller must hold fat32_io_lock (needed for fat32_next_cluster_cached).
// Returns 1 on success, 0 on failure (past end of chain or alloc failure).
static int ic_chain_extend(ic_inode_t *inode, unsigned long target_idx,
                           fat32_fs_t *fs)
{
    // Seed with start_cluster if empty
    if (inode->chain_len == 0) {
        if (!inode->chain) {
            unsigned long cap = CC_INIT_CAP;
            inode->chain = (unsigned long *)kalloc(cap * sizeof(unsigned long));
            if (!inode->chain)
                return 0;
            inode->chain_cap = cap;
        }
        inode->chain[0] = inode->start_cluster;
        inode->chain_len = 1;
    }

    while (inode->chain_len <= target_idx) {
        unsigned long last = inode->chain[inode->chain_len - 1];
        unsigned long next = fat32_next_cluster_cached(fs, last);
        if (next >= 0x0FFFFFF8 || next == 0)
            return 0; // end of chain

        // Grow array if needed
        if (inode->chain_len >= inode->chain_cap) {
            unsigned long new_cap = inode->chain_cap * 2;
            if (new_cap > CC_MAX_CAP)
                new_cap = CC_MAX_CAP;
            if (new_cap <= inode->chain_cap)
                return 0; // can't grow
            unsigned long *new_arr = (unsigned long *)kalloc(new_cap * sizeof(unsigned long));
            if (!new_arr)
                return 0;
            for (unsigned long i = 0; i < inode->chain_len; i++)
                new_arr[i] = inode->chain[i];
            kfree(inode->chain);
            inode->chain     = new_arr;
            inode->chain_cap = new_cap;
        }

        inode->chain[inode->chain_len] = next;
        inode->chain_len++;
    }
    return 1;
}

unsigned long icache_chain_get(unsigned long start_cluster, unsigned long idx,
                               struct fat32_fs *fs_raw)
{
    if (!ic_initialized || start_cluster < 2 || !fs_raw)
        return 0;

    fat32_fs_t *fs = (fat32_fs_t *)fs_raw;

    // Look up the inode
    ic_inode_t *inode = icache_lookup(start_cluster);
    if (!inode) {
        // No cached inode — fall back to linear walk
        unsigned long cur = start_cluster;
        for (unsigned long i = 0; i < idx; i++) {
            unsigned long next = fat32_next_cluster_cached(fs, cur);
            if (next >= 0x0FFFFFF8 || next == 0)
                return 0;
            cur = next;
        }
        return cur;
    }

    // Fast path: already populated
    if (idx < inode->chain_len)
        return inode->chain[idx];

    // Slow path: extend the chain under fat32_io_lock (caller should already
    // hold it, but ic_chain_extend only reads the FAT cache which is fine).
    if (!ic_chain_extend(inode, idx, fs))
        return 0;

    return inode->chain[idx];
}

void icache_chain_invalidate(unsigned long start_cluster)
{
    if (!ic_initialized || start_cluster < 2)
        return;

    ic_inode_t *inode = icache_lookup(start_cluster);
    if (!inode)
        return;

    if (inode->chain) {
        kfree(inode->chain);
        inode->chain     = 0;
        inode->chain_len = 0;
        inode->chain_cap = 0;
    }
}

// ============================================================================
// Per-inode I/O lock
// ============================================================================

void icache_io_lock(ic_inode_t *inode)
{
    if (!inode)
        return;
    while (1) {
        uint64_t flags;
        spin_lock_irqsave(&inode->io_wait_lock, &flags);
        if (!inode->io_locked) {
            inode->io_locked = 1;
            spin_unlock_irqrestore(&inode->io_wait_lock, flags);
            return;
        }
        task_t *cur = sched_current();
        if (cur) {
            cur->state = TASK_BLOCKED;
            cur->wait_channel = &inode->io_locked;
        }
        spin_unlock_irqrestore(&inode->io_wait_lock, flags);
        sched_schedule();
    }
}

void icache_io_unlock(ic_inode_t *inode)
{
    if (!inode)
        return;
    uint64_t flags;
    spin_lock_irqsave(&inode->io_wait_lock, &flags);
    inode->io_locked = 0;
    spin_unlock_irqrestore(&inode->io_wait_lock, flags);
    sched_wake_channel(&inode->io_locked);
}

// ============================================================================
// Statistics
// ============================================================================

void icache_get_stats(ic_stats_t *stats)
{
    if (!stats)
        return;
    stats->hits          = ic_stat_hits;
    stats->misses        = ic_stat_misses;
    stats->evictions     = ic_stat_evictions;
    stats->total_entries = ic_entry_count;
}
