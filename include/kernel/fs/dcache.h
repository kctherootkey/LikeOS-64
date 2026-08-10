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

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>

// ============================================================================
// Configuration
// ============================================================================

#define DC_HASH_BUCKETS 1024
#define DC_HASH_MASK (DC_HASH_BUCKETS - 1)

// Maximum cached dentries before LRU eviction kicks in
#define DC_MAX_ENTRIES 4096

// Maximum filename length we cache (longer names bypass the cache)
#define DC_NAME_MAX 255

// ============================================================================
// Dentry flags
// ============================================================================

#define DC_VALID 0x01 // Entry is valid
#define DC_NEGATIVE 0x02 // Negative entry (name does not exist)
#define DC_DIRECTORY 0x04 // Entry is a directory
/* This entry was made through the case-sensitive interface below, and is
 * found only through it.  The two interfaces hash and compare names
 * differently, so an entry made by one is not addressable by the other; the
 * flag makes that a stated rule rather than a consequence. */
#define DC_CASE_SENSITIVE 0x08

// ============================================================================
// Structures
// ============================================================================

typedef struct dc_entry {
	// Cache key
	unsigned long parent_cluster; // Parent directory's start_cluster
	unsigned long name_hash; // Hash of the filename (case-insensitive)

	// Cached result (valid only if !(flags & DC_NEGATIVE))
	unsigned long start_cluster; // File/dir's first cluster
	unsigned long size; // File size in bytes
	unsigned int attr; // FAT32 attributes
	uint16_t wrt_time; // FAT32 write time
	uint16_t wrt_date; // FAT32 write date

	// Directory entry location (for dirent updates)
	unsigned long dirent_cluster; // Cluster containing the short entry
	unsigned int dirent_index; // Index within that cluster
	unsigned long lfn_start_cluster; // First LFN entry cluster
	unsigned int lfn_start_index; // First LFN entry index

	// The actual name (case-preserved)
	char name[DC_NAME_MAX + 1];

	// State
	uint32_t flags;

	// Hash chain (per-bucket singly-linked list)
	struct dc_entry *hash_next;

	// Global LRU doubly-linked list
	struct dc_entry *lru_prev;
	struct dc_entry *lru_next;
} dc_entry_t;

// ============================================================================
// API
// ============================================================================

// Initialize the dentry cache (call during kernel init)
void dcache_init(void);

// Look up a dentry. Returns a pointer to the cached entry, or NULL on miss.
// If the returned entry has DC_NEGATIVE set, the name was looked up before
// and confirmed not to exist.
dc_entry_t *dcache_lookup(unsigned long parent_cluster, const char *name);

// Insert a positive dentry (found result) into the cache.
void dcache_insert(unsigned long parent_cluster, const char *name,
		   unsigned long start_cluster, unsigned long size,
		   unsigned int attr, uint16_t wrt_time, uint16_t wrt_date,
		   unsigned long dirent_cluster, unsigned int dirent_index,
		   unsigned long lfn_start_cluster,
		   unsigned int lfn_start_index);

// Insert a negative dentry (not-found result) into the cache.
void dcache_insert_negative(unsigned long parent_cluster, const char *name);

// Invalidate all dentries whose parent is `parent_cluster`.
// Called when a directory's contents change (create/unlink/rename/mkdir/rmdir).
void dcache_invalidate_dir(unsigned long parent_cluster);

// Invalidate a specific dentry by name.
void dcache_invalidate(unsigned long parent_cluster, const char *name);

// Invalidate the entire cache (e.g., filesystem unmount).
void dcache_invalidate_all(void);

// ============================================================================
// Case-sensitive interface
// ============================================================================

/* The entry points above fold case, which is what FAT32 needs and what ext4
 * must not have: on ext4 "Makefile" and "makefile" are two different files,
 * and a cache that cannot tell them apart answers one with the other.
 *
 * These are the same cache with the folding removed.  Nothing else about it
 * changes -- same buckets, same LRU, same eviction, and the two invalidation
 * calls above work on entries of either kind, because they key on the parent
 * alone.
 *
 * Entries made through here are found only through here (DC_CASE_SENSITIVE),
 * and the two sets are kept apart by their keys as well: ext4 tags its parent
 * ids, so an inode number and a FAT32 cluster number never collide.
 */

/* What a lookup answers with.
 *
 * Copied out under the cache's own lock rather than handed back as a pointer.
 * dcache_lookup() returns the entry itself, which is only safe for as long as
 * nothing evicts it -- and eviction runs on whichever processor inserts next,
 * so "as long as" is not a duration the caller can establish.  The value is
 * three words; copying it removes the question entirely.
 */
typedef struct dc_result {
	unsigned long ino; // The filesystem's id for the name
	unsigned long size; // Size in bytes, if the filesystem tracks it here
	unsigned int attr; // Filesystem-defined; bit 0x10 == directory
} dc_result_t;

#define DC_LOOKUP_MISS 0 // Nothing cached for this name
#define DC_LOOKUP_FOUND 1 // Cached, and it exists (*out filled in)
#define DC_LOOKUP_NEGATIVE 2 // Cached, and it is known NOT to exist

// Look up `name` in `parent`, comparing case-sensitively.
int dcache_lookup_cs(unsigned long parent, const char *name, dc_result_t *out);

// Record that `name` in `parent` exists and names `ino`.
void dcache_insert_cs(unsigned long parent, const char *name, unsigned long ino,
		      unsigned long size, unsigned int attr);

// Record that `name` in `parent` does not exist.
void dcache_insert_negative_cs(unsigned long parent, const char *name);

// Forget whatever was recorded for `name` in `parent`.
void dcache_invalidate_cs(unsigned long parent, const char *name);

// Statistics
typedef struct dc_stats {
	uint64_t hits; // Positive cache hits
	uint64_t neg_hits; // Negative cache hits
	uint64_t misses; // Cache misses
	uint64_t insertions; // Total insertions
	uint64_t evictions; // LRU evictions
	uint64_t total_entries; // Current cached entries
} dc_stats_t;

void dcache_get_stats(dc_stats_t *stats);

// Bytes currently held by cached dentries; reclaimable via LRU eviction,
// reported by sysinfo as cache memory.
uint64_t dcache_mem_bytes(void);

#endif // _KERNEL_DCACHE_H_
