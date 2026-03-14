// LikeOS-64 Inode Cache
//
// Caches per-file metadata (size, attributes, dirent location) indexed by
// start_cluster — FAT32's de-facto inode number.
//
// Each cached inode carries a reference count: open file handles hold refs,
// and the inode stays cached even after all handles close (for quick reopen).
// A per-inode lock enables concurrent reads to different cached files
// without requiring the global fat32_io_lock.
//
// SMP-safe with per-bucket spinlocks and per-inode locks.

#ifndef _KERNEL_ICACHE_H_
#define _KERNEL_ICACHE_H_

#include "types.h"
#include "sched.h"

// ============================================================================
// Configuration
// ============================================================================

#define IC_HASH_BUCKETS     512
#define IC_HASH_MASK        (IC_HASH_BUCKETS - 1)

// Maximum cached inodes before LRU eviction of zero-refcount entries
#define IC_MAX_ENTRIES      2048

// ============================================================================
// Inode flags
// ============================================================================

#define IC_VALID            0x01    // Inode is valid
#define IC_DIRTY            0x02    // Metadata modified, needs writeback

// ============================================================================
// Structures
// ============================================================================

typedef struct ic_inode {
    // Key
    unsigned long   start_cluster;      // File's first cluster (inode number)

    // Cached metadata
    unsigned long   size;               // File size in bytes
    unsigned int    attr;               // FAT32 attributes
    unsigned long   parent_cluster;     // Parent directory cluster
    unsigned long   dirent_cluster;     // Cluster containing the short dirent
    unsigned int    dirent_index;       // Index of short dirent in that cluster
    uint16_t        wrt_time;           // FAT32 write time
    uint16_t        wrt_date;           // FAT32 write date

    // Reference counting
    volatile int    refcount;           // Number of open file handles

    // State
    uint32_t        flags;

    // Per-inode lock: allows concurrent cached reads to different files
    // without the global fat32_io_lock.
    volatile int    io_locked;
    spinlock_t      io_wait_lock;

    // Cluster chain cache — linearized array of cluster numbers.
    // chain[0] = start_cluster, chain[1] = next, etc.
    // Lazily populated on demand.  Avoids O(N) chain walks.
    unsigned long  *chain;              // Cluster number array
    unsigned long   chain_len;          // Number of valid entries
    unsigned long   chain_cap;          // Allocated capacity

    // Hash chain (per-bucket singly-linked)
    struct ic_inode* hash_next;

    // LRU doubly-linked list (for eviction of zero-refcount inodes)
    struct ic_inode* lru_prev;
    struct ic_inode* lru_next;
} ic_inode_t;

// ============================================================================
// API
// ============================================================================

// Initialize the inode cache (call during kernel init)
void icache_init(void);

// Look up an inode by start_cluster. Returns NULL on miss.
// Does NOT increment refcount — caller must call icache_ref() if keeping.
ic_inode_t* icache_lookup(unsigned long start_cluster);

// Look up or create: returns an inode for start_cluster.
// If not cached, allocates a new inode with the given metadata.
// Increments refcount.
ic_inode_t* icache_get(unsigned long start_cluster, unsigned long size,
                       unsigned int attr, unsigned long parent_cluster,
                       unsigned long dirent_cluster, unsigned int dirent_index,
                       uint16_t wrt_time, uint16_t wrt_date);

// Increment reference count
void icache_ref(ic_inode_t *inode);

// Decrement reference count (inode stays cached even at refcount 0)
void icache_unref(ic_inode_t *inode);

// Update inode metadata (e.g., after write changes file size)
void icache_update_size(ic_inode_t *inode, unsigned long new_size);

// Mark inode dirty (metadata needs writeback to disk)
void icache_mark_dirty(ic_inode_t *inode);

// Flush a dirty inode's metadata to disk (writes the dirent back)
int icache_flush(ic_inode_t *inode);

// Flush all dirty inodes
int icache_flush_all(void);

// Remove an inode from the cache (on unlink)
void icache_remove(unsigned long start_cluster);

// Invalidate the entire cache
void icache_invalidate_all(void);

// Per-inode I/O lock (for cached-read-without-global-lock pattern)
void icache_io_lock(ic_inode_t *inode);
void icache_io_unlock(ic_inode_t *inode);

// --- Cluster chain cache ---

// Get the cluster number at chain index `idx` for a file identified by
// start_cluster.  Looks up the inode and lazily extends its cached chain.
// Returns the cluster number, or 0 if past end-of-chain / error.
// `fs` is needed to walk the FAT on a cache miss.
struct fat32_fs;  // forward declaration
unsigned long icache_chain_get(unsigned long start_cluster, unsigned long idx,
                               struct fat32_fs *fs);

// Invalidate (discard) the chain cache for a file.
// Call on truncate or unlink.
void icache_chain_invalidate(unsigned long start_cluster);

// Statistics
typedef struct ic_stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t total_entries;
} ic_stats_t;

void icache_get_stats(ic_stats_t *stats);

#endif // _KERNEL_ICACHE_H_
