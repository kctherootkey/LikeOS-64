// LikeOS-64 Dentry Cache
//
// Caches directory lookup results keyed by (parent_cluster, name_hash)
// so that repeated open()/stat()/access() calls on the same path skip
// the expensive FAT32 directory-cluster scanning.
//
// Supports negative dentries (not-found results) to speed up $PATH
// lookups where most candidates miss.
//
// SMP-safe with per-bucket spinlocks.

#ifndef _KERNEL_DCACHE_H_
#define _KERNEL_DCACHE_H_

#include "types.h"
#include "sched.h"

// ============================================================================
// Configuration
// ============================================================================

#define DC_HASH_BUCKETS     1024
#define DC_HASH_MASK        (DC_HASH_BUCKETS - 1)

// Maximum cached dentries before LRU eviction kicks in
#define DC_MAX_ENTRIES      4096

// Maximum filename length we cache (longer names bypass the cache)
#define DC_NAME_MAX         255

// ============================================================================
// Dentry flags
// ============================================================================

#define DC_VALID            0x01    // Entry is valid
#define DC_NEGATIVE         0x02    // Negative entry (name does not exist)
#define DC_DIRECTORY        0x04    // Entry is a directory

// ============================================================================
// Structures
// ============================================================================

typedef struct dc_entry {
    // Cache key
    unsigned long   parent_cluster;     // Parent directory's start_cluster
    unsigned long   name_hash;          // Hash of the filename (case-insensitive)

    // Cached result (valid only if !(flags & DC_NEGATIVE))
    unsigned long   start_cluster;      // File/dir's first cluster
    unsigned long   size;               // File size in bytes
    unsigned int    attr;               // FAT32 attributes
    uint16_t        wrt_time;           // FAT32 write time
    uint16_t        wrt_date;           // FAT32 write date

    // Directory entry location (for dirent updates)
    unsigned long   dirent_cluster;     // Cluster containing the short entry
    unsigned int    dirent_index;       // Index within that cluster
    unsigned long   lfn_start_cluster;  // First LFN entry cluster
    unsigned int    lfn_start_index;    // First LFN entry index

    // The actual name (case-preserved)
    char            name[DC_NAME_MAX + 1];

    // State
    uint32_t        flags;

    // Hash chain (per-bucket singly-linked list)
    struct dc_entry* hash_next;

    // Global LRU doubly-linked list
    struct dc_entry* lru_prev;
    struct dc_entry* lru_next;
} dc_entry_t;

// ============================================================================
// API
// ============================================================================

// Initialize the dentry cache (call during kernel init)
void dcache_init(void);

// Look up a dentry. Returns a pointer to the cached entry, or NULL on miss.
// If the returned entry has DC_NEGATIVE set, the name was looked up before
// and confirmed not to exist.
dc_entry_t* dcache_lookup(unsigned long parent_cluster, const char *name);

// Insert a positive dentry (found result) into the cache.
void dcache_insert(unsigned long parent_cluster, const char *name,
                   unsigned long start_cluster, unsigned long size,
                   unsigned int attr, uint16_t wrt_time, uint16_t wrt_date,
                   unsigned long dirent_cluster, unsigned int dirent_index,
                   unsigned long lfn_start_cluster, unsigned int lfn_start_index);

// Insert a negative dentry (not-found result) into the cache.
void dcache_insert_negative(unsigned long parent_cluster, const char *name);

// Invalidate all dentries whose parent is `parent_cluster`.
// Called when a directory's contents change (create/unlink/rename/mkdir/rmdir).
void dcache_invalidate_dir(unsigned long parent_cluster);

// Invalidate a specific dentry by name.
void dcache_invalidate(unsigned long parent_cluster, const char *name);

// Invalidate the entire cache (e.g., filesystem unmount).
void dcache_invalidate_all(void);

// Statistics
typedef struct dc_stats {
    uint64_t hits;          // Positive cache hits
    uint64_t neg_hits;      // Negative cache hits
    uint64_t misses;        // Cache misses
    uint64_t insertions;    // Total insertions
    uint64_t evictions;     // LRU evictions
    uint64_t total_entries; // Current cached entries
} dc_stats_t;

void dcache_get_stats(dc_stats_t *stats);

#endif // _KERNEL_DCACHE_H_
