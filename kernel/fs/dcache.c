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

#include <kernel/fs/dcache.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// Hash table bucket
// ============================================================================

typedef struct {
	dc_entry_t *head;
	spinlock_t lock;
} dc_bucket_t;

static dc_bucket_t dc_hash[DC_HASH_BUCKETS];

// ============================================================================
// Global LRU list (doubly-linked circular with sentinel)
// ============================================================================

static dc_entry_t dc_lru_sentinel;
static spinlock_t dc_lru_lock;
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
	dc_entry_count = 0;
	dc_stat_hits = 0;
	dc_stat_neg_hits = 0;
	dc_stat_misses = 0;
	dc_stat_insertions = 0;
	dc_stat_evictions = 0;
	dc_initialized = 1;
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
		if (ca >= 'A' && ca <= 'Z')
			ca += 32;
		if (cb >= 'A' && cb <= 'Z')
			cb += 32;
		if (ca != cb)
			return ca - cb;
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

// ============================================================================
// Case-sensitive name hash and comparison
// ============================================================================

/* Same FNV-1a, without the folding.  A filesystem where "Makefile" and
 * "makefile" are two files needs the cache to tell them apart, and folding
 * the hash would put them in the same bucket for the comparison to sort out
 * -- which the folding comparison then also gets wrong. */
static unsigned long dc_name_hash_cs(const char *name)
{
	unsigned long h = 14695981039346656037UL; // FNV offset basis
	for (int i = 0; name[i]; i++) {
		h ^= (unsigned char)name[i];
		h *= 1099511628211UL; // FNV prime
	}
	return h;
}

static int dc_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

/* The two interfaces differ only in how a name is hashed and compared, plus
 * the flag that keeps their entries from being addressed by the wrong one. */
static inline int dc_key_matches(const dc_entry_t *e, unsigned long parent,
				 unsigned long nh, const char *name, int cs)
{
	if (e->parent_cluster != parent || e->name_hash != nh)
		return 0;
	if (!!(e->flags & DC_CASE_SENSITIVE) != !!cs)
		return 0;
	return cs ? (dc_strcmp(e->name, name) == 0) :
		    (dc_strcasecmp(e->name, name) == 0);
}

/* A name the cache cannot hold in full must not be cached at all.
 *
 * dc_alloc_entry truncates, which is harmless as long as nothing is ever
 * looked up by the truncated form -- but two distinct names sharing a
 * DC_NAME_MAX-byte prefix would then become the same entry, and one would be
 * answered with the other's inode.  Refusing is a missed cache hit; aliasing
 * is the wrong file. */
static inline int dc_name_fits(const char *name)
{
	int i = 0;

	while (name[i] && i <= DC_NAME_MAX)
		i++;
	return i <= DC_NAME_MAX;
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
	WARN_ON_ONCE((victim->flags & DC_VALID) &&
		     !(victim->flags & DC_NEGATIVE) &&
		     victim->start_cluster == 1);
	unsigned long bucket =
		dc_bucket_index(victim->parent_cluster, victim->name_hash);
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

dc_entry_t *dcache_lookup(unsigned long parent_cluster, const char *name)
{
	BUG_ON(name == NULL);
	WARN_ON_ONCE(
		!dc_initialized); /* dcache_lookup called before dcache_init - init ordering bug */
	if (!dc_initialized || !name)
		return 0;

	unsigned long nh = dc_name_hash(name);
	unsigned long bucket = dc_bucket_index(parent_cluster, nh);

	uint64_t flags;
	spin_lock_irqsave(&dc_hash[bucket].lock, &flags);

	dc_entry_t *e = dc_hash[bucket].head;
	while (e) {
		if (dc_key_matches(e, parent_cluster, nh, name, 0)) {
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

static dc_entry_t *dc_alloc_entry(unsigned long parent_cluster,
				  const char *name, unsigned long nh)
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

/* Free a chain of entries already detached from both the hash and the LRU.
 *
 * The freeing is deliberately NOT done while the bucket lock is held.  kfree
 * can return a slab page to the allocator, which unmaps it and shoots down the
 * other processors' TLBs -- and it waits for them to acknowledge.  A processor
 * spinning on this same bucket lock has interrupts disabled and cannot
 * acknowledge anything, so the two wait for each other permanently.
 *
 * Nothing can reach these entries in the meantime: they are off the hash chain
 * and off the LRU, so neither a lookup nor an eviction can find them.  The
 * `hash_next` field, no longer a hash chain, links them together here. */
static void dc_free_chain(dc_entry_t *victims)
{
	while (victims) {
		dc_entry_t *next = victims->hash_next;

		kfree(victims);
		__sync_fetch_and_sub(&dc_entry_count, 1);
		victims = next;
	}
}

/* Remove EVERY entry with this key from hash+LRU.
 *
 * Every, not the first, because two of them can exist.  Inserting is a remove
 * followed by an allocate followed by a publish, and the allocate can sleep,
 * so the bucket lock cannot be held across the three.  Two processors looking
 * up the same missing name therefore both remove nothing, both allocate, and
 * both publish -- two entries, same key.  They always agree (a lookup runs
 * with the filesystem's metadata lock held shared, so no mutation can be in
 * flight while either of them reads the directory), which makes the duplicate
 * harmless to read.
 *
 * It is not harmless to remove.  Stopping at the first would leave the second
 * behind, and that leftover is a name that has just been deleted still
 * answering lookups. */
static void dc_remove_existing(unsigned long parent_cluster, const char *name,
			       unsigned long nh, unsigned long bucket, int cs)
{
	dc_entry_t *victims = 0;
	uint64_t flags;

	spin_lock_irqsave(&dc_hash[bucket].lock, &flags);
	dc_entry_t **pp = &dc_hash[bucket].head;
	while (*pp) {
		dc_entry_t *e = *pp;
		if (dc_key_matches(e, parent_cluster, nh, name, cs)) {
			*pp = e->hash_next;

			uint64_t lru_flags;
			spin_lock_irqsave(&dc_lru_lock, &lru_flags);
			dc_lru_remove(e);
			spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

			e->hash_next = victims;
			victims = e;
			continue; /* *pp now names the next entry */
		}
		pp = &(*pp)->hash_next;
	}
	spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

	dc_free_chain(victims);
}

/* Make a fully-populated entry reachable: hash chain, LRU head, counters.
 *
 * The last step of every insert and identical in all of them, so it is one
 * function -- the ordering (reachable only once its fields are set) is the
 * part worth stating in one place rather than three. */
static void dc_publish(dc_entry_t *e, unsigned long bucket)
{
	uint64_t flags;

	spin_lock_irqsave(&dc_hash[bucket].lock, &flags);
	e->hash_next = dc_hash[bucket].head;
	dc_hash[bucket].head = e;
	spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

	uint64_t lru_flags;
	spin_lock_irqsave(&dc_lru_lock, &lru_flags);
	dc_lru_add(e);
	spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

	__sync_fetch_and_add(&dc_entry_count, 1);
	__sync_fetch_and_add(&dc_stat_insertions, 1);
}

// ============================================================================
// Insert positive dentry
// ============================================================================

void dcache_insert(unsigned long parent_cluster, const char *name,
		   unsigned long start_cluster, unsigned long size,
		   unsigned int attr, uint16_t wrt_time, uint16_t wrt_date,
		   unsigned long dirent_cluster, unsigned int dirent_index,
		   unsigned long lfn_start_cluster,
		   unsigned int lfn_start_index)
{
	BUG_ON(name == NULL);
	BUG_ON(name != NULL && name[0] == '\0');
	if (!dc_initialized || !name || !name[0])
		return;

	unsigned long nh = dc_name_hash(name);
	unsigned long bucket = dc_bucket_index(parent_cluster, nh);

	// Remove any existing entry with same key
	dc_remove_existing(parent_cluster, name, nh, bucket, 0);

	dc_entry_t *e = dc_alloc_entry(parent_cluster, name, nh);
	if (!e)
		return;

	e->start_cluster = start_cluster;
	WARN_ON(start_cluster != 0 &&
		start_cluster <
			2); /* files must use cluster >= 2 (0 == root placeholder) */
	WARN_ON(start_cluster ==
		1); /* inode-id 1 is reserved (FAT32 cluster 1); never a valid file id */
	e->size = size;
	e->attr = attr;
	e->wrt_time = wrt_time;
	e->wrt_date = wrt_date;
	e->dirent_cluster = dirent_cluster;
	e->dirent_index = dirent_index;
	e->lfn_start_cluster = lfn_start_cluster;
	e->lfn_start_index = lfn_start_index;
	e->flags = DC_VALID;
	/* Bit 0x10 == directory in the FS-defined attribute encoding.  The
     * dcache itself only needs to know "is this a directory" to set the
     * DC_DIRECTORY hint; the FS driver chooses an encoding that places
     * the directory bit at 0x10 when registering entries. */
	if (attr & 0x10)
		e->flags |= DC_DIRECTORY;

	dc_publish(e, bucket);
	WARN_ON_ONCE(
		dc_entry_count >
		DC_MAX_ENTRIES +
			1); /* dcache_insert: entry count exceeded limit after insert: eviction loop broken */
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
	dc_remove_existing(parent_cluster, name, nh, bucket, 0);

	dc_entry_t *e = dc_alloc_entry(parent_cluster, name, nh);
	if (!e)
		return;

	e->flags = DC_VALID | DC_NEGATIVE;

	dc_publish(e, bucket);
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

	dc_remove_existing(parent_cluster, name, nh, bucket, 0);
}

void dcache_invalidate_dir(unsigned long parent_cluster)
{
	if (!dc_initialized)
		return;

	// Scan all buckets — O(n) but invalidation is infrequent.
	for (int b = 0; b < DC_HASH_BUCKETS; b++) {
		dc_entry_t *victims = 0;
		uint64_t flags;

		spin_lock_irqsave(&dc_hash[b].lock, &flags);

		dc_entry_t **pp = &dc_hash[b].head;
		while (*pp) {
			dc_entry_t *e = *pp;
			if (e->parent_cluster == parent_cluster) {
				*pp = e->hash_next;

				/* Off the LRU before the bucket lock is
				 * dropped, not after: an entry that is off the
				 * hash but still on the LRU can be picked by a
				 * concurrent eviction, which would fail to find
				 * it in its bucket and free it -- while it is
				 * also on this list, waiting to be freed. */
				uint64_t lru_flags;
				spin_lock_irqsave(&dc_lru_lock, &lru_flags);
				dc_lru_remove(e);
				spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

				e->hash_next = victims;
				victims = e;
			} else {
				pp = &(*pp)->hash_next;
			}
		}
		spin_unlock_irqrestore(&dc_hash[b].lock, flags);

		dc_free_chain(victims);
	}
}

void dcache_invalidate_all(void)
{
	if (!dc_initialized)
		return;

	/* Empty the LRU first, so that no eviction can pick an entry this is
	 * about to free out from under it. */
	uint64_t lru_flags;
	spin_lock_irqsave(&dc_lru_lock, &lru_flags);
	dc_lru_sentinel.lru_prev = &dc_lru_sentinel;
	dc_lru_sentinel.lru_next = &dc_lru_sentinel;
	spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

	for (int b = 0; b < DC_HASH_BUCKETS; b++) {
		dc_entry_t *victims;
		uint64_t flags;

		spin_lock_irqsave(&dc_hash[b].lock, &flags);
		victims = dc_hash[b].head;
		dc_hash[b].head = 0;
		spin_unlock_irqrestore(&dc_hash[b].lock, flags);

		/* Freed with the bucket lock released -- see dc_free_chain. */
		while (victims) {
			dc_entry_t *next = victims->hash_next;

			victims->lru_prev = 0;
			victims->lru_next = 0;
			kfree(victims);
			victims = next;
		}
	}

	dc_entry_count = 0;
}

// ============================================================================
// Case-sensitive interface
// ============================================================================

/* The same cache, addressed without folding case.  See the header for why a
 * filesystem that distinguishes "Makefile" from "makefile" cannot share the
 * folding entry points above.
 *
 * Only the hash and the comparison differ; everything else -- buckets, LRU,
 * eviction, the two invalidate-by-parent calls -- is shared, and entries of
 * the two kinds sit side by side in the same table without meeting, because
 * DC_CASE_SENSITIVE is part of the key match.
 */

int dcache_lookup_cs(unsigned long parent, const char *name, dc_result_t *out)
{
	BUG_ON(name == NULL);
	BUG_ON(out == NULL);
	if (!dc_initialized || !name || !out)
		return DC_LOOKUP_MISS;
	/* Nothing longer than the cache holds is ever inserted, so nothing
	 * longer than that can be found; skip the walk. */
	if (!dc_name_fits(name))
		return DC_LOOKUP_MISS;

	unsigned long nh = dc_name_hash_cs(name);
	unsigned long bucket = dc_bucket_index(parent, nh);
	int result = DC_LOOKUP_MISS;

	uint64_t flags;
	spin_lock_irqsave(&dc_hash[bucket].lock, &flags);
	for (dc_entry_t *e = dc_hash[bucket].head; e; e = e->hash_next) {
		if (!dc_key_matches(e, parent, nh, name, 1))
			continue;

		uint64_t lru_flags;
		spin_lock_irqsave(&dc_lru_lock, &lru_flags);
		dc_lru_touch(e);
		spin_unlock_irqrestore(&dc_lru_lock, lru_flags);

		/* Copied out here, under the bucket lock, and not a moment
		 * later: once this lock is dropped the entry may be evicted by
		 * any processor that inserts, and the caller would be reading
		 * freed memory. */
		if (e->flags & DC_NEGATIVE) {
			result = DC_LOOKUP_NEGATIVE;
		} else {
			out->ino = e->start_cluster;
			out->size = e->size;
			out->attr = e->attr;
			result = DC_LOOKUP_FOUND;
		}
		break;
	}
	spin_unlock_irqrestore(&dc_hash[bucket].lock, flags);

	if (result == DC_LOOKUP_FOUND)
		__sync_fetch_and_add(&dc_stat_hits, 1);
	else if (result == DC_LOOKUP_NEGATIVE)
		__sync_fetch_and_add(&dc_stat_neg_hits, 1);
	else
		__sync_fetch_and_add(&dc_stat_misses, 1);
	return result;
}

void dcache_insert_cs(unsigned long parent, const char *name, unsigned long ino,
		      unsigned long size, unsigned int attr)
{
	BUG_ON(name == NULL);
	BUG_ON(parent == 0);
	if (!dc_initialized || !name || !name[0])
		return;
	/* Truncating would alias two names that share a DC_NAME_MAX-byte
	 * prefix onto one entry, and answer one with the other's inode.  Not
	 * caching it costs a directory scan; aliasing costs the wrong file. */
	if (WARN_ON(!dc_name_fits(name)))
		return;

	unsigned long nh = dc_name_hash_cs(name);
	unsigned long bucket = dc_bucket_index(parent, nh);

	dc_remove_existing(parent, name, nh, bucket, 1);

	dc_entry_t *e = dc_alloc_entry(parent, name, nh);
	if (!e)
		return;

	e->start_cluster = ino;
	e->size = size;
	e->attr = attr;
	e->flags = DC_VALID | DC_CASE_SENSITIVE;
	if (attr & 0x10)
		e->flags |= DC_DIRECTORY;

	dc_publish(e, bucket);
	WARN_ON_ONCE(dc_entry_count > DC_MAX_ENTRIES + 1);
}

void dcache_insert_negative_cs(unsigned long parent, const char *name)
{
	BUG_ON(name == NULL);
	BUG_ON(parent == 0);
	if (!dc_initialized || !name || !name[0])
		return;
	if (WARN_ON(!dc_name_fits(name)))
		return;

	unsigned long nh = dc_name_hash_cs(name);
	unsigned long bucket = dc_bucket_index(parent, nh);

	dc_remove_existing(parent, name, nh, bucket, 1);

	dc_entry_t *e = dc_alloc_entry(parent, name, nh);
	if (!e)
		return;

	e->flags = DC_VALID | DC_NEGATIVE | DC_CASE_SENSITIVE;

	dc_publish(e, bucket);
}

void dcache_invalidate_cs(unsigned long parent, const char *name)
{
	BUG_ON(name == NULL);
	if (!dc_initialized || !name || !name[0])
		return;
	if (!dc_name_fits(name))
		return; /* never inserted, so never present */

	unsigned long nh = dc_name_hash_cs(name);
	unsigned long bucket = dc_bucket_index(parent, nh);

	dc_remove_existing(parent, name, nh, bucket, 1);
}

// ============================================================================
// Statistics
// ============================================================================

void dcache_get_stats(dc_stats_t *stats)
{
	if (!stats)
		return;
	stats->hits = dc_stat_hits;
	stats->neg_hits = dc_stat_neg_hits;
	stats->misses = dc_stat_misses;
	stats->insertions = dc_stat_insertions;
	stats->evictions = dc_stat_evictions;
	stats->total_entries = dc_entry_count;
}

uint64_t dcache_mem_bytes(void)
{
	return dc_entry_count * sizeof(dc_entry_t);
}
