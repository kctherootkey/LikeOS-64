// LikeOS-64 Inode Cache
//
// Caches per-file metadata indexed by the FS-native inode identifier (FAT32
// start cluster, EXT4 inode number).  Reference counted: open handles hold
// refs; inodes stay cached after close.  Per-inode I/O locks enable
// concurrent cached reads without the FS-wide I/O lock.
//
// Hash table with per-bucket spinlocks.  LRU list for evicting
// zero-refcount inodes when the cache is full.
//
// All FS-specific I/O dispatches through vfs_superblock_t->ops, so this file
// references no FAT32 (or EXT4) symbols directly.

#include "../../include/kernel/icache.h"
#include "../../include/kernel/vfs_sb.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/bug.h"
#include "../../include/kernel/dcache.h"

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
    BUILD_BUG_ON(IC_HASH_BUCKETS == 0);
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
        WARN_ON_ONCE(1);  /* eviction called on empty LRU - cache accounting bug */
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

    BUG_ON(victim->refcount > 0);  /* evicting inode still in use: LRU contains a referenced inode, data corruption will follow */

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
    might_sleep();
    VM_BUG_ON(start_cluster < 2);
    WARN_ON(start_cluster == 1);  /* block-id 1 is reserved in FAT32; treated as invalid generically */
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
            WARN_ON_ONCE(n->refcount < 0);  /* negative refcount before icache_get bump: use-after-free or icache_unref overcounted */
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
    WARN_ON(start_cluster < 2);  /* inode start_cluster must be >= 2 */
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
    BUG_ON(inode == NULL);
    if (!inode)
        return;
    int old = __sync_fetch_and_add(&inode->refcount, 1);
    WARN_ON(old < 0);  /* refcount was negative - use-after-free or corruption */
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
    WARN_ON(new_rc < -1);  /* refcount went below -1 - severe double-unref or corruption */
    if (new_rc <= 0) {
        // Refcount reached zero — add to LRU for possible eviction
        if (new_rc < 0) {
            WARN(1, "icache_unref: refcount underflow on start_cluster=%lu", inode->start_cluster);
            inode->refcount = 0; // clamp
        }
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
    if (!g_root_sb)
        return -1;

    /* Dispatch through the generic superblock op; the FS driver knows how
     * its inode metadata is laid out on disk.  Returns 0 on success. */
    int rc = vfs_sb_write_inode(g_root_sb, inode);
    if (rc != 0)
        return rc;

    inode->flags &= ~IC_DIRTY;
    /* The dcache caches size from the on-disk dirent at lookup time.
     * After mutating that dirent the dcache must be invalidated or
     * subsequent stat() lookups will return the stale pre-flush size. */
    if (inode->parent_cluster)
        dcache_invalidate_dir(inode->parent_cluster);
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
// Caller must hold the FS-wide I/O lock (sb->ops->lock_io()) because
// sb->ops->next_block() touches the on-disk allocator state.
// Returns 1 on success, 0 on failure (past end of chain or alloc failure).
static int ic_chain_extend(ic_inode_t *inode, unsigned long target_idx,
                           vfs_superblock_t *sb)
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

    unsigned long eoc = vfs_sb_end_of_chain(sb);
    while (inode->chain_len <= target_idx) {
        unsigned long last = inode->chain[inode->chain_len - 1];
        unsigned long next = vfs_sb_next_block(sb, last);
        if (next == 0 || next >= eoc)
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
                               struct vfs_superblock *sb)
{
    if (!ic_initialized || start_cluster < 2 || !sb)
        return 0;

    unsigned long eoc = vfs_sb_end_of_chain(sb);

    // Look up the inode
    ic_inode_t *inode = icache_lookup(start_cluster);
    if (!inode) {
        // No cached inode — fall back to linear walk via sb ops.
        unsigned long cur = start_cluster;
        for (unsigned long i = 0; i < idx; i++) {
            unsigned long next = vfs_sb_next_block(sb, cur);
            if (next == 0 || next >= eoc)
                return 0;
            cur = next;
        }
        return cur;
    }

    // Fast path: already populated
    if (idx < inode->chain_len)
        return inode->chain[idx];

    // Slow path: extend the chain.  Caller is expected to hold the FS-wide
    // I/O lock; ic_chain_extend only reads through sb ops.
    if (!ic_chain_extend(inode, idx, sb))
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
