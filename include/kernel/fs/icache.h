// LikeOS-64 Inode Cache
//
// Caches per-file metadata (size, attributes, dirent location) indexed by
// the filesystem's native inode identifier.  For FAT32 that is the file's
// first cluster; for EXT4 it is the inode-table index.  The cache treats
// the key as an opaque `unsigned long` and never dereferences any FS-
// specific state — all I/O dispatches through vfs_superblock_t->ops.
//
// Each cached inode carries a reference count: open file handles hold refs,
// and the inode stays cached even after all handles close (for quick reopen).
// A per-inode lock enables concurrent reads to different cached files
// without requiring the global per-FS I/O lock.
//
// SMP-safe with per-bucket spinlocks and per-inode locks.

#ifndef _KERNEL_ICACHE_H_
#define _KERNEL_ICACHE_H_

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>

struct vfs_superblock; /* forward — see vfs_sb.h                    */

// ============================================================================
// Configuration
// ============================================================================

#define IC_HASH_BUCKETS 512
#define IC_HASH_MASK (IC_HASH_BUCKETS - 1)

// Maximum cached inodes before LRU eviction of zero-refcount entries
#define IC_MAX_ENTRIES 2048

// ============================================================================
// Inode flags
// ============================================================================

#define IC_VALID 0x01 // Inode is valid
#define IC_DIRTY 0x02 // Metadata modified, needs writeback
#define IC_DEAD                                            \
	0x04 // Removed from cache while still referenced; \
		// freed by the last icache_unref()
#define IC_SETID_CLEAN                                      \
	0x08 // No set-user/-group-ID bits left to strip on \
		// a non-privileged modify (write-path fast \
		// path); cleared whenever the mode changes.

// ============================================================================
// Structures
// ============================================================================

typedef struct ic_inode {
	// Key — FS-native inode identifier (FAT32: start cluster; EXT4: inode no.).
	unsigned long start_cluster;

	// Generic cached metadata
	unsigned long size; // File size in bytes
	unsigned int attr; // FS-defined attribute bits (opaque to caches)
	unsigned long parent_cluster; // Parent directory's inode identifier
	uint16_t wrt_time; // FS-defined mtime encoding (opaque)
	uint16_t wrt_date;

	/* FS-private metadata location.  For FAT32 the on-disk dirent lives at
     * (dirent_cluster, dirent_index); for other filesystems these may
     * encode an inode-table block + offset, or be unused entirely.  The
     * caches only pass these to sb->ops->write_inode(); they never
     * interpret them. */
	unsigned long dirent_cluster;
	unsigned int dirent_index;

	// Reference counting
	volatile int refcount; // Number of open file handles

	// State
	uint32_t flags;

	// Per-inode lock: allows concurrent cached reads to different files
	// without holding the FS-wide I/O lock.
	volatile int io_locked;
	spinlock_t io_wait_lock;

	// Block-chain cache — linearized array of FS-native block_ids that hold
	// this inode's data.  chain[0] = start_cluster, chain[1] = next, etc.
	// Lazily populated on demand to avoid O(N) chain walks.
	unsigned long *chain;
	unsigned long chain_len;
	unsigned long chain_cap;

	// Hash chain (per-bucket singly-linked)
	struct ic_inode *hash_next;

	// LRU doubly-linked list (for eviction of zero-refcount inodes)
	struct ic_inode *lru_prev;
	struct ic_inode *lru_next;
} ic_inode_t;

// ============================================================================
// API
// ============================================================================

// Initialize the inode cache (call during kernel init)
void icache_init(void);

// Look up an inode by start_cluster. Returns NULL on miss.
// Does NOT increment refcount — caller must call icache_ref() if keeping.
ic_inode_t *icache_lookup(unsigned long start_cluster);

// Look up or create: returns an inode for start_cluster.
// If not cached, allocates a new inode with the given metadata.
// Increments refcount.
ic_inode_t *icache_get(unsigned long start_cluster, unsigned long size,
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

// --- Block chain cache ---

// Get the FS-native block_id at chain index `idx` for a file identified by
// start_cluster.  Looks up the inode and lazily extends its cached chain
// via sb->ops->next_block().  Returns the block_id, or 0 if past
// end-of-chain / error.
unsigned long icache_chain_get(unsigned long start_cluster, unsigned long idx,
			       struct vfs_superblock *sb);

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
