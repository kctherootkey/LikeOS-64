// LikeOS-64 Dentry Cache
//
// Caches the results of directory lookups keyed by
// (parent_inode_id, case-insensitive name hash).  parent_inode_id is
// the filesystem's native parent identifier — FAT32 parent cluster,
// EXT4 parent inode number — treated here as an opaque `unsigned long`.
// The cache only stores results returned by the filesystem driver and
// never interprets the on-disk format itself; FS-specific metadata
// like dirent location is opaque to this layer.
//
// This eliminates redundant directory scanning for repeated
// open()/stat()/access() calls on the same path.  Negative dentries
// cache "not found" results to accelerate $PATH resolution where most
// candidate paths miss.
//
// Uses a hash table with per-bucket spinlocks for SMP safety.
// A global LRU doubly-linked list provides eviction ordering when
// the cache exceeds DC_MAX_ENTRIES.

#include <kernel/dcache.h>
#include <kernel/memory.h>
#include <kernel/console.h>
#include <kernel/bug.h>

// ============================================================================
// Hash table bucket
// ============================================================================

typedef struct {
    dc_entry_t*  head;
    spinlock_t   lock;
} dc_bucket_t;

static dc_bucket_t dc_hash[DC_HASH_BUCKETS];

// ============================================================================
// Global LRU list (doubly-linked circular with sentinel)
// ============================================================================

static dc_entry_t  dc_lru_sentinel;
static spinlock_t  dc_lru_lock;
static volatile uint64_t dc_entry_count;

// ============================================================================
// Statistics
// ============================================================================

static volatile uint64_t dc_stat_hits;
static volatile uint64_t dc_stat_neg_hits;
static volatile uint64_t dc_stat_misses;
static volatile uint64_t dc_stat_insertions;
static volatile uint64_t dc_stat_evictions;

// ============================================================================
// Initialization
// ============================================================================

static int dc_initialized = 0;

void dcache_init(void)
{
    BUILD_BUG_ON(DC_HASH_BUCKETS == 0);
    for (int i = 0; i < DC_HASH_BUCKETS; i++) {
        dc_hash[i].head = 0;
        spinlock_init(&dc_hash[i].lock, "dcache");
    }
    dc_lru_sentinel.lru_prev = &dc_lru_sentinel;
    dc_lru_sentinel.lru_next = &dc_lru_sentinel;
    spinlock_init(&dc_lru_lock, "dc_lru");
    dc_entry_count  = 0;
    dc_stat_hits    = 0;
    dc_stat_neg_hits = 0;
    dc_stat_misses  = 0;
    dc_stat_insertions = 0;
    dc_stat_evictions  = 0;
    dc_initialized  = 1;
}

// ============================================================================
// Case-insensitive name hash (FNV-1a)
// ============================================================================

static unsigned long dc_name_hash(const char *name)
{
    unsigned long h = 14695981039346656037UL; // FNV offset basis
    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z')
            c += 32; // lowercase
        h ^= (unsigned char)c;
        h *= 1099511628211UL; // FNV prime
    }
    return h;
}

// Case-insensitive string comparison
static int dc_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

// ============================================================================
// LRU helpers (caller must hold dc_lru_lock)
// ============================================================================

static void dc_lru_add(dc_entry_t *e)
{
    VM_BUG_ON(e == NULL);
    // Add to head of LRU (most recently used)
    e->lru_next = dc_lru_sentinel.lru_next;
    e->lru_prev = &dc_lru_sentinel;
    dc_lru_sentinel.lru_next->lru_prev = e;
    dc_lru_sentinel.lru_next = e;
}

static void dc_lru_remove(dc_entry_t *e)
{
    if (e->lru_prev)
        e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next)
        e->lru_next->lru_prev = e->lru_prev;
    e->lru_prev = 0;
    e->lru_next = 0;
}

// Move to head of LRU (most recently used)
static void dc_lru_touch(dc_entry_t *e)
{
    dc_lru_remove(e);
    dc_lru_add(e);
}

// ============================================================================
// Hash helpers
// ============================================================================

static inline unsigned long dc_bucket_index(unsigned long parent_cluster,
                                            unsigned long name_hash)
{
    return (parent_cluster * 2654435761UL ^ name_hash) & DC_HASH_MASK;
}

// ============================================================================
// Eviction — remove LRU tail entry when cache is full
// ============================================================================

static void dc_evict_one(void)
{
    // Pick the LRU tail (least recently used)
    uint64_t lru_flags;
    spin_lock_irqsave(&dc_lru_lock, &lru_flags);

    dc_entry_t *victim = dc_lru_sentinel.lru_prev;
    if (victim == &dc_lru_sentinel) {
        spin_unlock_irqrestore(&dc_lru_lock, lru_flags);
        return; // empty
    }
    dc_lru_remove(victim);
    spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

    /* Valid non-negative entry whose inode-id is 1 indicates filesystem
     * corruption — on FAT32 cluster 1 is reserved; other filesystems also
     * treat 0/1 as out-of-band. */
    WARN_ON_ONCE((victim->flags & DC_VALID) && !(victim->flags & DC_NEGATIVE) && victim->start_cluster == 1);
    unsigned long bucket = dc_bucket_index(victim->parent_cluster,
                                           victim->name_hash);
    uint64_t bucket_flags;
    spin_lock_irqsave(&dc_hash[bucket].lock, &bucket_flags);
    dc_entry_t **pp = &dc_hash[bucket].head;
    while (*pp) {
        if (*pp == victim) {
            *pp = victim->hash_next;
            break;
        }
        pp = &(*pp)->hash_next;
    }
    spin_unlock_irqrestore(&dc_hash[bucket].lock, bucket_flags);

    kfree(victim);
    __sync_fetch_and_sub(&dc_entry_count, 1);
    __sync_fetch_and_add(&dc_stat_evictions, 1);
}

// ============================================================================
// Lookup
// ============================================================================

dc_entry_t* dcache_lookup(unsigned long parent_cluster, const char *name)
{
    BUG_ON(name == NULL);
    WARN_ON_ONCE(!dc_initialized);  /* dcache_lookup called before dcache_init - init ordering bug */
    if (!dc_initialized || !name)
        return 0;

    unsigned long nh = dc_name_hash(name);
    unsigned long bucket = dc_bucket_index(parent_cluster, nh);

    uint64_t flags;
    spin_lock_irqsave(&dc_hash[bucket].lock, &flags);

    dc_entry_t *e = dc_hash[bucket].head;
    while (e) {
        if (e->parent_cluster == parent_cluster &&
            e->name_hash == nh &&
            dc_strcasecmp(e->name, name) == 0) {
            // Hit — move to LRU head
            uint64_t lru_flags;
            spin_lock_irqsave(&dc_lru_lock, &lru_flags);
            dc_lru_touch(e);
            spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

            spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

            if (e->flags & DC_NEGATIVE)
                __sync_fetch_and_add(&dc_stat_neg_hits, 1);
            else
                __sync_fetch_and_add(&dc_stat_hits, 1);
            return e;
        }
        e = e->hash_next;
    }
    spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);
    __sync_fetch_and_add(&dc_stat_misses, 1);
    return 0;
}

// ============================================================================
// Internal insert helper
// ============================================================================

static dc_entry_t* dc_alloc_entry(unsigned long parent_cluster, const char *name,
                                   unsigned long nh)
{
    might_sleep();
    BUG_ON(name == NULL);
    // Evict if at capacity
    while (dc_entry_count >= DC_MAX_ENTRIES)
        dc_evict_one();

    dc_entry_t *e = (dc_entry_t *)kalloc(sizeof(dc_entry_t));
    if (!e)
        return 0;
    mm_memset(e, 0, sizeof(dc_entry_t));
    e->parent_cluster = parent_cluster;
    e->name_hash = nh;
    // Copy name (case-preserved)
    int i;
    for (i = 0; name[i] && i < DC_NAME_MAX; i++)
        e->name[i] = name[i];
    e->name[i] = '\0';
    return e;
}

// Remove existing entry with same key from hash+LRU (if any)
static void dc_remove_existing(unsigned long parent_cluster, const char *name,
                                unsigned long nh, unsigned long bucket)
{
    uint64_t flags;
    spin_lock_irqsave(&dc_hash[bucket].lock, &flags);
    dc_entry_t **pp = &dc_hash[bucket].head;
    while (*pp) {
        dc_entry_t *e = *pp;
        if (e->parent_cluster == parent_cluster &&
            e->name_hash == nh &&
            dc_strcasecmp(e->name, name) == 0) {
            *pp = e->hash_next;
            spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

            uint64_t lru_flags;
            spin_lock_irqsave(&dc_lru_lock, &lru_flags);
            dc_lru_remove(e);
            spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

            kfree(e);
            __sync_fetch_and_sub(&dc_entry_count, 1);
            return;
        }
        pp = &(*pp)->hash_next;
    }
    spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);
}

// ============================================================================
// Insert positive dentry
// ============================================================================

void dcache_insert(unsigned long parent_cluster, const char *name,
                   unsigned long start_cluster, unsigned long size,
                   unsigned int attr, uint16_t wrt_time, uint16_t wrt_date,
                   unsigned long dirent_cluster, unsigned int dirent_index,
                   unsigned long lfn_start_cluster, unsigned int lfn_start_index)
{
    BUG_ON(name == NULL);
    BUG_ON(name != NULL && name[0] == '\0');
    if (!dc_initialized || !name || !name[0])
        return;

    unsigned long nh = dc_name_hash(name);
    unsigned long bucket = dc_bucket_index(parent_cluster, nh);

    // Remove any existing entry with same key
    dc_remove_existing(parent_cluster, name, nh, bucket);

    dc_entry_t *e = dc_alloc_entry(parent_cluster, name, nh);
    if (!e)
        return;

    e->start_cluster    = start_cluster;
    WARN_ON(start_cluster != 0 && start_cluster < 2);  /* files must use cluster >= 2 (0 == root placeholder) */
    WARN_ON(start_cluster == 1);  /* inode-id 1 is reserved (FAT32 cluster 1); never a valid file id */
    e->size             = size;
    e->attr             = attr;
    e->wrt_time         = wrt_time;
    e->wrt_date         = wrt_date;
    e->dirent_cluster   = dirent_cluster;
    e->dirent_index     = dirent_index;
    e->lfn_start_cluster = lfn_start_cluster;
    e->lfn_start_index  = lfn_start_index;
    e->flags            = DC_VALID;
    /* Bit 0x10 == directory in the FS-defined attribute encoding.  The
     * dcache itself only needs to know "is this a directory" to set the
     * DC_DIRECTORY hint; the FS driver chooses an encoding that places
     * the directory bit at 0x10 when registering entries. */
    if (attr & 0x10)
        e->flags |= DC_DIRECTORY;

    // Insert into hash bucket
    uint64_t flags;
    spin_lock_irqsave(&dc_hash[bucket].lock, &flags);
    e->hash_next = dc_hash[bucket].head;
    dc_hash[bucket].head = e;
    spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

    // Add to LRU head
    uint64_t lru_flags;
    spin_lock_irqsave(&dc_lru_lock, &lru_flags);
    dc_lru_add(e);
    spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

    __sync_fetch_and_add(&dc_entry_count, 1);
    WARN_ON_ONCE(dc_entry_count > DC_MAX_ENTRIES + 1);  /* dcache_insert: entry count exceeded limit after insert: eviction loop broken */
    __sync_fetch_and_add(&dc_stat_insertions, 1);
}

// ============================================================================
// Insert negative dentry
// ============================================================================

void dcache_insert_negative(unsigned long parent_cluster, const char *name)
{
    if (!dc_initialized || !name || !name[0])
        return;

    unsigned long nh = dc_name_hash(name);
    unsigned long bucket = dc_bucket_index(parent_cluster, nh);

    // Remove any existing entry with same key
    dc_remove_existing(parent_cluster, name, nh, bucket);

    dc_entry_t *e = dc_alloc_entry(parent_cluster, name, nh);
    if (!e)
        return;

    e->flags = DC_VALID | DC_NEGATIVE;

    // Insert into hash bucket
    uint64_t flags;
    spin_lock_irqsave(&dc_hash[bucket].lock, &flags);
    e->hash_next = dc_hash[bucket].head;
    dc_hash[bucket].head = e;
    spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

    // Add to LRU head
    uint64_t lru_flags;
    spin_lock_irqsave(&dc_lru_lock, &lru_flags);
    dc_lru_add(e);
    spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

    __sync_fetch_and_add(&dc_entry_count, 1);
    __sync_fetch_and_add(&dc_stat_insertions, 1);
}

// ============================================================================
// Invalidation
// ============================================================================

void dcache_invalidate(unsigned long parent_cluster, const char *name)
{
    if (!dc_initialized || !name)
        return;

    unsigned long nh = dc_name_hash(name);
    unsigned long bucket = dc_bucket_index(parent_cluster, nh);

    uint64_t flags;
    spin_lock_irqsave(&dc_hash[bucket].lock, &flags);
    dc_entry_t **pp = &dc_hash[bucket].head;
    while (*pp) {
        dc_entry_t *e = *pp;
        if (e->parent_cluster == parent_cluster &&
            e->name_hash == nh &&
            dc_strcasecmp(e->name, name) == 0) {
            *pp = e->hash_next;
            spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

            uint64_t lru_flags;
            spin_lock_irqsave(&dc_lru_lock, &lru_flags);
            dc_lru_remove(e);
            spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

            kfree(e);
            __sync_fetch_and_sub(&dc_entry_count, 1);
            return;
        }
        pp = &(*pp)->hash_next;
    }
    spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);
}

void dcache_invalidate_dir(unsigned long parent_cluster)
{
    if (!dc_initialized)
        return;

    // Scan all buckets — O(n) but invalidation is infrequent.
    for (int b = 0; b < DC_HASH_BUCKETS; b++) {
        uint64_t flags;
        spin_lock_irqsave(&dc_hash[b].lock, &flags);

        dc_entry_t **pp = &dc_hash[b].head;
        while (*pp) {
            dc_entry_t *e = *pp;
            if (e->parent_cluster == parent_cluster) {
                *pp = e->hash_next;

                uint64_t lru_flags;
                spin_lock_irqsave(&dc_lru_lock, &lru_flags);
                dc_lru_remove(e);
                spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

                kfree(e);
                __sync_fetch_and_sub(&dc_entry_count, 1);
            } else {
                pp = &(*pp)->hash_next;
            }
        }
        spin_unlock_irqrestore(&dc_hash[b].lock, flags);
    }
}

void dcache_invalidate_all(void)
{
    if (!dc_initialized)
        return;

    for (int b = 0; b < DC_HASH_BUCKETS; b++) {
        uint64_t flags;
        spin_lock_irqsave(&dc_hash[b].lock, &flags);

        dc_entry_t *e = dc_hash[b].head;
        while (e) {
            dc_entry_t *next = e->hash_next;
            kfree(e);
            e = next;
        }
        dc_hash[b].head = 0;
        spin_unlock_irqrestore(&dc_hash[b].lock, flags);
    }

    uint64_t lru_flags;
    spin_lock_irqsave(&dc_lru_lock, &lru_flags);
    dc_lru_sentinel.lru_prev = &dc_lru_sentinel;
    dc_lru_sentinel.lru_next = &dc_lru_sentinel;
    spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

    dc_entry_count = 0;
}

// ============================================================================
// Statistics
// ============================================================================

void dcache_get_stats(dc_stats_t *stats)
{
    if (!stats)
        return;
    stats->hits          = dc_stat_hits;
    stats->neg_hits      = dc_stat_neg_hits;
    stats->misses        = dc_stat_misses;
    stats->insertions    = dc_stat_insertions;
    stats->evictions     = dc_stat_evictions;
    stats->total_entries = dc_entry_count;
}
