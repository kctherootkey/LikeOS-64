/* LikeOS - exFAT filesystem driver: declarations shared between its files
 *
 * Conventions shared with the NTFS and ISO9660 drivers, locking, write
 * safety and mount options: exfat.h.  The driver is organised top-down:
 *
 *    1. configuration, in-memory types
 *    2. byte helpers, checksums (boot region, entry set, name hash, upcase)
 *    3. the lock engine (private recursive rwsem, lock wrappers)
 *    4. device I/O: bounded reads, THE write chokepoint, discard, sync,
 *       the guard block device the page cache writes through
 *    5. error policy (errors=), corruption reports
 *    6. boot region: parse, checksum, geometry validation, read-only gates
 *    7. metadata cache: FAT region and allocation bitmap units
 *    8. FAT entries and chain decoding into extents
 *    9. allocation bitmap: test / set / free runs, free count, allocator,
 *       deferred frees and deferred FAT writes
 *   10. upcase table, UTF-8 <-> UTF-16, folding, NameHash
 *   11. timestamps
 *   12. streams: handle table, key table, extents, LRU, lifetime
 *   13. page-cache superblock operations (block_at & co.)
 *   14. directory reading: iterator, entry-set parsing and validation
 *   15. name lookup (dcache), path resolution
 *   16. directory modification: building entry sets, the directory write
 *       queue, free-slot search, directory growth
 *   17. the commit machinery: arming, ordered commit, clean marking
 *   18. cluster chains of streams: grow, convert, shrink, release
 *   19. ValidDataLength, the data read / write paths
 *   20. VFS operations
 *   21. mount options
 *   22. mount, unmount, remount, probe, the filesystem type
 *
 * The sections live in these files of kernel/fs/exfat/:
 *
 *   super.c   1 (the configuration; the in-memory types are in this
 *             header), 6, 17, 20 (whole-filesystem operations, the
 *             operation table), 21, 22
 *   misc.c    2, 3, 4, 5, 11
 *   cache.c   7
 *   fatent.c  8, 18
 *   balloc.c  9
 *   nls.c     10
 *   inode.c   12, 13
 *   dir.c     14, 16, 20 (readdir)
 *   namei.c   15, 20 (create, unlink, rmdir, mkdir, rename, chdir)
 *   file.c    19, 20 (stat, open, close, read, write, seek, truncate,
 *             fsync, attributes and times)
 *
 * The code follows the exFAT file system specification; it carries no code
 * from any other implementation.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_EXFAT_INT_H
#define LIKEOS_EXFAT_INT_H

/* ===========================================================================
 * 1. Configuration and in-memory types
 * =========================================================================== */

/* Hashed per-stream rwsems (ext4's per-inode locks): a collision merely
 * serialises two unrelated streams. */
#define EXFAT_SLOCKS 64

/* Buckets of the stream key table. */
#define EXFAT_KHASH 1024

/* Unreferenced streams kept for a quick reopen before the oldest is
 * forgotten (its pages dropped, its handle freed). */
#define EXFAT_STREAM_LRU_MAX 1024

/* Largest extent array one stream may decode to (16 bytes each: 16 MB for
 * a file fragmented into a million pieces).  Beyond it the stream is
 * refused with ST_NOMEM rather than taking unbounded memory. */
#define EXFAT_MAX_EXTENTS (1u << 20)

/* Metadata cache: units of the FAT region and of the allocation bitmap,
 * min(cluster size, 4096) bytes each.  A soft cap: dirty units cannot be
 * dropped, so the cache may exceed it until the next commit writes them. */
#define EXFAT_MC_MAX 512
#define EXFAT_MC_HASH 256
/* FAT readahead: units read in one transfer on a miss, so that walking a
 * long chain costs one device command per 64 KB of FAT. */
#define EXFAT_MC_READAHEAD 16

/* Deferred frees (runs) gathered before the commit that applies them.  The
 * operation that would exceed it commits early (always consistent). */
#define EXFAT_PENDING_FREE_MAX 8192
/* Deferred FAT writes (truncation end-of-chain marks). */
#define EXFAT_DEFERRED_FAT_MAX 1024

/* Directory write queue: cached directory pages changed by the running
 * operation, written at the commit. */
#define EXFAT_DIRW_MAX 32

/* Largest single device transfer the driver issues (sectors of 512 bytes
 * at least; the device's own sector size scales it). */
#define EXFAT_MAX_IO_BYTES (256UL << 10)

/* Page size as a shift (PAGE_SIZE is 4096). */
#ifndef PAGE_SHIFT
#define PAGE_SHIFT 12
#endif

/* Path walks: components and ancestors remembered (rename's loop check). */
#define EXFAT_DEPTH_MAX 128

/* Largest buffer the zeroing helper writes with. */
#define EXFAT_ZERO_CHUNK (64UL << 10)

/* Grown streams whose FAT links one commit writes in two steps. */
#define EXFAT_LINKS_MAX 32

/* Writes copy at most this many pages before balancing dirty memory. */
#define EXFAT_BALANCE_EVERY 64

/* fsync() waits this many rounds for a writeback pass holding pages of the
 * file before it reports that the file could not be stored. */
#define EXFAT_FSYNC_TRIES 1000

/* Directory ".." / "." and directory slots: readdir positions 0 and 1 are
 * the dot entries, slot N of the directory is position N + 2. */
#define EXFAT_DIRPOS_FIRST 2

/* Reasons a mount is read-only although read-write was asked for. */
enum exfat_ro_reason {
	EXFAT_RO_NONE = 0,
	EXFAT_RO_REQUESTED, /* "ro" */
	EXFAT_RO_DEVICE, /* write-protected device */
	EXFAT_RO_BACKUP_BOOT, /* main boot region invalid, backup used */
	EXFAT_RO_REVISION, /* not revision 1.x of the format */
	EXFAT_RO_TWO_FATS, /* TexFAT volume (two FATs) */
	EXFAT_RO_DIRTY, /* VolumeDirty set, no "force" */
	EXFAT_RO_MEDIA_FAILURE, /* MediaFailure flag set */
	EXFAT_RO_GEOMETRY, /* regions not aligned to device sectors */
	EXFAT_RO_NO_BITMAP, /* allocation bitmap missing / unusable */
	EXFAT_RO_UPCASE, /* upcase table missing or its checksum bad */
	EXFAT_RO_ERRORS, /* errors=remount-ro fired */
	EXFAT_RO_OWN_FREE, /* the volume's own clusters marked free */
	EXFAT_RO_NREASONS
};

/* errors= */
#define EXFAT_ERRORS_RO 0
#define EXFAT_ERRORS_CONTINUE 1
#define EXFAT_ERRORS_PANIC 2

/* Default permission masks (exfat.h: umask 022). */
#define EXFAT_DEFAULT_MASK 0022u

/* Mount options (section 21). */
struct exfat_opts {
	uint32_t uid;
	uint32_t gid;
	uint32_t fmask;
	uint32_t dmask;
	uint32_t allow_utime; /* 0xFFFFFFFF: not given */
	int perm_opts; /* any of uid=/gid=/umask=/fmask=/dmask= given */
	int errors; /* EXFAT_ERRORS_* */
	int discard;
	int force;
	int time_local; /* time=local: timestamps without a UTC offset
			 * are local time (time_offset minutes from UTC) */
	int time_offset; /* minutes east of UTC, -720 .. 840 */
	int iocharset_given;
};

/* ---- The private recursive rwsem (ext4's semantics, own code) ----------- */

/* Tasks whose shared holds one rwsem records at a time. */
#define EXFAT_RWSEM_TRACK 32
typedef struct exfat_rwsem {
	volatile int readers; /* shared holders */
	volatile int writer; /* 1 while held exclusive */
	volatile uint64_t owner; /* exclusive owner's task id */
	volatile int wdepth; /* exclusive recursion depth */
	volatile int w_wait; /* writers queued */
	volatile int r_wait; /* readers queued (the cohort) */
	volatile int reader_turn; /* the queued cohort goes before writers */
	/* Nonzero while held in either mode: what lockdep_assert_held()
	 * looks at. */
	volatile int locked;
	/* Shared holds, attributed to their tasks (EXFAT_RWSEM_TRACK tasks
	 * at a time; holds beyond that are only counted), so that
	 * release_locks_for_task() can take a dead task's shared holds back
	 * as well as its exclusive one. */
	int untracked;
	struct {
		uint64_t task;
		int count; /* 0: slot free */
	} rh[EXFAT_RWSEM_TRACK];
	spinlock_t lock; /* protects the fields above */
	const char *name;
} exfat_rwsem_t;

/* One extent: `len' clusters of the stream starting at stream cluster
 * index `fidx' live at disk clusters `disk' .. `disk' + len - 1. */
typedef struct exfat_extent {
	uint32_t fidx;
	uint32_t disk;
	uint32_t len;
	uint32_t _pad;
} exfat_extent_t;

/* A stream: one file or directory (exfat.h "streams"). */
typedef struct exfat_stream exfat_stream_t;
struct exfat_stream {
	struct exfat_fs *fs;
	uint32_t handle; /* page-cache / icache chain handle */
	volatile int refs; /* under fs->st_lock */
	/* Location key (parent directory's first cluster, File entry index);
	 * EXFAT_KEY_ROOT for the root directory; meaningless once orphaned. */
	uint64_t key;
	exfat_stream_t *parent; /* held; NULL for the root and orphans */
	uint32_t entry_index; /* File entry slot in the parent */
	uint8_t secondary_count; /* of the entry set on disk */

	/* What the entry set says (and what will be written back). */
	uint16_t attr;
	uint8_t sflags; /* GeneralSecondaryFlags of the stream entry */
	uint32_t first_cluster;
	uint64_t size; /* DataLength */
	uint64_t vdl; /* ValidDataLength */
	uint32_t ctime, mtime, atime; /* packed timestamps */
	uint8_t ctime_cs, mtime_cs; /* 10 ms increments */
	uint8_t ctime_utc, mtime_utc, atime_utc;

	/* The allocation, decoded. */
	uint32_t nclusters;
	exfat_extent_t *ext;
	uint32_t next;
	uint32_t ext_cap;

	unsigned is_dir : 1;
	unsigned is_root : 1;
	unsigned orphan : 1; /* unlinked; freed at the last close */
	unsigned orphan_held : 1; /* counted in fs->orphans_held */
	unsigned meta_dirty : 1; /* entry set differs from memory */
	unsigned data_dirty : 1; /* pages dirtied since the last flush */
	unsigned on_dirty_list : 1;
	unsigned on_lru : 1;
	unsigned hashed : 1; /* in the key table */
	unsigned grew : 1; /* DataLength/VDL grew since the last commit */
	unsigned dead : 1; /* removed from the tables, being freed */

	int opens; /* open handles (under meta excl or st_lock) */

	/* Cached count of subdirectories (stat's st_nlink), -1 unknown. */
	long subdirs;

	/* Pages dirtied since the last commit stored them: [dlo, dhi]
	 * (dhi < dlo: none). */
	unsigned long dlo, dhi;
	/* A write-back error of this stream's data, for fsync / close. */
	int wb_err;

	exfat_stream_t *hnext; /* key table chain */
	exfat_stream_t *lru_prev, *lru_next;
	exfat_stream_t *dnext; /* dirty list */
};

#define EXFAT_KEY_ROOT 0xFFFFFFFFFFFFFFFFULL
#define EXFAT_KEY(dir_cluster, idx) \
	((((uint64_t)(dir_cluster)) << 24) | ((uint64_t)(idx) & 0xFFFFFFULL))

/* Metadata cache unit. */
#define EXFAT_MC_FAT 1
#define EXFAT_MC_BITMAP 2
typedef struct exfat_mcu {
	uint8_t region; /* EXFAT_MC_* */
	uint8_t dirty; /* bit per device sector (at most 8 per unit) */
	uint16_t nsect; /* device sectors in this unit */
	uint32_t unit; /* unit index within the region */
	uint32_t nbytes; /* valid bytes (nsect * dev_ss) */
	volatile int refs;
	uint64_t lba; /* first device sector */
	uint8_t *data;
	struct exfat_mcu *hnext;
	struct exfat_mcu *lru_prev, *lru_next;
} exfat_mcu_t;

/* A queued directory write: bytes [lo, hi) of a held cached page of a
 * directory stream, written at the next commit. */
struct exfat_dirw {
	pc_page_t *pg;
	exfat_stream_t *d; /* held */
	unsigned long pidx;
	unsigned lo, hi;
};

/* A deferred free: `len' clusters from `start'. */
struct exfat_run {
	uint32_t start;
	uint32_t len;
};

/* A deferred FAT write. */
struct exfat_fatw {
	uint32_t cluster;
	uint32_t value;
};

typedef struct exfat_fs exfat_fs_t;
struct exfat_fs {
	/* The page cache's view.  sb.fs_private points back here. */
	vfs_superblock_t sb;
	/* The device the page cache sees: writes go through the chokepoint
	 * (section 4).  Found back from its address with offsetof. */
	block_device_t guard;
	block_device_t *bdev; /* the real device */
	vfs_mount_t *mnt;

	/* ---- geometry (fixed after mount) ---- */
	unsigned dev_ss; /* device sector bytes */
	unsigned dev_shift;
	unsigned fs_shift; /* BytesPerSectorShift */
	unsigned spc_shift; /* SectorsPerClusterShift */
	unsigned cs_shift; /* cluster bytes = 1 << cs_shift */
	unsigned long cs;
	unsigned bs_shift; /* exposed block bytes = 1 << bs_shift */
	unsigned long bs;
	unsigned bpc_shift; /* exposed blocks per cluster */
	unsigned long mcu_bytes; /* metadata cache unit size */
	uint64_t vol_dev_sectors; /* sectors of the volume on the device */
	uint64_t fat_lba; /* the FAT used, in device sectors */
	uint64_t fat_bytes; /* bytes of one FAT */
	uint64_t heap_lba; /* cluster 2, in device sectors */
	uint32_t cluster_count;
	uint32_t root_cluster;
	uint32_t serial;
	uint16_t revision;
	uint16_t vol_flags_mount; /* VolumeFlags as found at mount */
	int nfats;
	int active_fat;
	uint8_t percent_mount;

	/* ---- allocation bitmap ---- */
	uint32_t bm_first;
	uint64_t bm_bytes; /* DataLength of the bitmap */
	exfat_extent_t *bm_ext;
	uint32_t bm_next;
	int have_bitmap;

	/* ---- upcase table: 65536 entries, identity where not given ---- */
	uint16_t *upcase;
	int upcase_builtin; /* the volume had none usable */

	/* ---- informational ---- */
	uint16_t label[EXFAT_LABEL_MAX];
	unsigned label_len;

	/* ---- state ---- */
	int ro; /* read-only: requested or gated */
	unsigned gates; /* bit (1 << enum exfat_ro_reason) per reason */
	int ro_latched; /* errors=remount-ro fired while mounted */
	volatile int armed; /* VolumeDirty is on the device: writes go */
	/* A mutation passed the write checks: the chokepoint may arm the
	 * mount (write VolumeDirty) right before the first real write.  An
	 * operation that ends up changing nothing writes nothing. */
	int may_write;
	int meta_bits_checked; /* own clusters checked as marked used */
	int in_commit; /* the commit runs: never nested */
	uint64_t io_errors; /* failed or refused writes (sticky count) */
	/* Clusters were left allocated that nothing references (frees that
	 * could not be completed): VolumeDirty stays set, fsck's turn.  A
	 * failed write alone does not set it -- what it was to store stays
	 * dirty and is stored again (and the clean marking needs that). */
	int needs_check;
	int check_logged, free_fail_logged;
	uint8_t *boot_buf; /* exfat_boot_write_flags(), meta lock exclusive */
	/* Unlinked files still open whose clusters are freed at their last
	 * close (meta lock exclusive): no read-only remount meanwhile. */
	unsigned orphans_held;
	uint32_t up_first; /* upcase table, for the allocation check */
	uint64_t up_len;
	int was_dirty; /* VolumeDirty was set when mounted (force) */
	int barrier_needed; /* direct writes since the last device sync */
	int root_zero_pending; /* root clusters zeroed, link not stored */
	/* Streams that grew since the FAT was last written: the old last
	 * cluster whose entry now links to the new clusters, and the entry's
	 * value before (exfat_mc_flush_links). */
	struct {
		uint32_t cluster, old;
	} links[EXFAT_LINKS_MAX];
	unsigned nlinks;
	int dir_backup_valid;
	uint64_t errors_seen;
	unsigned refused_logged; /* refused-write log lines so far */

	struct exfat_opts opts;

	/* ---- locks (exfat.h, section 4) ---- */
	exfat_rwsem_t meta;
	exfat_rwsem_t alloc;
	exfat_rwsem_t slock[EXFAT_SLOCKS];
	spinlock_t st_lock; /* stream tables, refs, LRU */
	spinlock_t mc_lock; /* metadata cache */

	/* ---- streams ---- */
	/* By handle: EXFAT_HDIR chunk pointers, chunks of EXFAT_HCHUNK slots
	 * allocated on first use.  A free slot holds the next free handle
	 * (tagged); hfree_head is the first one (0: none). */
	exfat_stream_t ***htab;
	uint32_t hfree_head;
	uint32_t next_handle; /* first never-used handle */
	exfat_stream_t *khash[EXFAT_KHASH];
	exfat_stream_t *lru_head, *lru_tail; /* head = most recent */
	unsigned lru_count;
	unsigned nstreams;
	exfat_stream_t *root;
	exfat_stream_t *dirty_head; /* streams with meta_dirty (held) */
	/* Orphans whose last reference went while the metadata lock was not
	 * held exclusive: destroyed at the end of the next exclusive hold
	 * (block_at() & co. use stream pointers under the lock in either
	 * mode, so only an exclusive holder may free one). */
	exfat_stream_t *reap_head;

	/* ---- metadata cache ---- */
	exfat_mcu_t *mc_hash[EXFAT_MC_HASH];
	exfat_mcu_t *mc_lru_head, *mc_lru_tail;
	unsigned mc_count;
	unsigned mc_dirty;

	/* ---- allocator ---- */
	int64_t free_clusters; /* -1: not counted yet */
	uint32_t alloc_cursor; /* next-fit start (cluster number) */

	/* ---- deferred work of the running commit window ---- */
	struct exfat_run *pf;
	unsigned npf, pf_cap;
	uint64_t pf_clusters;
	struct exfat_fatw *dfat;
	unsigned ndfat, dfat_cap;
	struct exfat_dirw dirw[EXFAT_DIRW_MAX];
	unsigned ndirw;

	/* ---- commit scratch (metadata lock exclusive) ---- */
	uint8_t wb_set[EXFAT_SET_MAX_BYTES];
	uint16_t wb_name[EXFAT_NAME_MAX + 1];

	/* ---- statistics (informational) ---- */
	uint64_t st_dev_writes;
	uint64_t st_dev_reads;
	uint64_t st_discards;
	uint64_t st_commits;
};

/* An open file or directory. */
typedef struct exfat_file {
	vfs_file_t vfs; /* first: the VFS's handle is ours */
	exfat_fs_t *fs;
	exfat_stream_t *st; /* held */
	ic_inode_t *ic; /* the inode cache's entry for the stream */
	unsigned long pos;
	uint64_t dirpos; /* readdir: 0 ".", 1 "..", N + 2 slot N */
	uint64_t parent_ino; /* ".." of a directory */
	pc_readahead_t ra;
	int open_flags;
	int wrote; /* written through this handle since the last commit */
	int werr; /* deferred write error for fsync / close */
} exfat_file_t;

/* Per-operation scratch: buffers too big for the kernel stack. */
typedef struct exfat_ctx {
	uint8_t set[EXFAT_SET_MAX_BYTES]; /* an entry set */
	uint8_t set2[EXFAT_SET_MAX_BYTES]; /* a second one (rename) */
	uint16_t name[EXFAT_NAME_MAX + 1]; /* a name, UTF-16 */
	uint16_t name2[EXFAT_NAME_MAX + 1];
	uint16_t up[EXFAT_NAME_MAX + 1]; /* upcased */
	char u8[EXFAT_NAME_MAX * 3 + 1]; /* a name, UTF-8 */
	char key[EXFAT_NAME_MAX * 3 + 1]; /* a folded dcache key */
	char key2[EXFAT_NAME_MAX * 3 + 1]; /* a second one (rename) */
	char comp[VFS_NAME_MAX + 1]; /* a path component */
	uint32_t anc[EXFAT_DEPTH_MAX]; /* ancestors' first clusters */
	unsigned nanc;
} exfat_ctx_t;

/* What a directory lookup found: one entry set. */
typedef struct exfat_dentry_info {
	uint32_t index; /* File entry slot */
	uint8_t secondary_count;
	uint16_t attr;
	uint8_t sflags;
	uint8_t name_len;
	uint16_t name_hash;
	uint32_t first_cluster;
	uint64_t size;
	uint64_t vdl;
	uint32_t ctime, mtime, atime;
	uint8_t ctime_cs, mtime_cs, ctime_utc, mtime_utc, atime_utc;
	/* Benign secondaries with an allocation (vendor allocation entries):
	 * freed with the set. */
	int has_alloc_secondaries;
} exfat_dentry_info_t;

/* Mutator entry check: the metadata lock is held exclusive by this task.
 * lockdep_assert_held() sees that the lock is held at all; the owner check
 * catches a shared hold where an exclusive one is required. */
#define exfat_assert_excl(fs)                                              \
	do {                                                               \
		lockdep_assert_held(&(fs)->meta);                          \
		WARN_ON_ONCE(!exfat_rwsem_held_excl(&(fs)->meta));         \
	} while (0)

#define exfat_assert_meta(fs)                                              \
	do {                                                               \
		lockdep_assert_held(&(fs)->meta);                          \
	} while (0)

/* What a write may touch. */
#define EXFAT_W_BOOT 1 /* the main boot sector: VolumeFlags, PercentInUse */
#define EXFAT_W_META 2 /* the FAT region and everything after it */
#define EXFAT_W_HEAP 3 /* the cluster heap only (file / directory data) */

/* Corrupt on-disk metadata met: rate-limited warning, the errors= policy,
 * and the status the caller returns. */
#define exfat_corrupt(fs, where, fmt, ...)                                  \
	({                                                                  \
		WARN_RATELIMIT(1, "exfat: corrupt volume: " fmt,            \
			       ##__VA_ARGS__);                              \
		exfat_fs_error((fs), "corrupt metadata", (uint32_t)(where)); \
		ST_IO;                                                      \
	})

#define EXFAT_UPCASE_ENTRIES 65536u

#define EXFAT_HCHUNK_SHIFT 12
#define EXFAT_HCHUNK (1u << EXFAT_HCHUNK_SHIFT)
#define EXFAT_HDIR ((uint32_t)((EXFAT_HANDLE_MAX + 1) >> EXFAT_HCHUNK_SHIFT))

typedef struct exfat_diter {
	exfat_fs_t *fs;
	exfat_stream_t *d;
	pc_page_t *pg;
	unsigned long pidx;
	int err;
} exfat_diter_t;

/* What a path resolves to. */
struct exfat_walk {
	exfat_stream_t *dir; /* held: directory of the last component
			      * (NULL when the path is the root) */
	exfat_stream_t *st; /* held: the object, when exfat_walk_obj() got it */
	exfat_dentry_info_t di; /* the last component's entry set */
	int found; /* the last component exists */
	int is_root;
	int trailing_slash; /* "name/" */
	const char *last; /* the last component in the path */
	unsigned long last_len;
	int nlen; /* the last component's length in UTF-16 units */
	uint16_t hash; /* its NameHash */
};

/* ===========================================================================
 * Functions and data shared between the files, by the file that defines them
 * =========================================================================== */

/* super.c */
extern const vfs_ops_t exfat_vfs_ops;
void exfat_stream_mark_dirty(exfat_fs_t *fs, exfat_stream_t *st);
int exfat_may_write(exfat_fs_t *fs);
int exfat_arm_now(exfat_fs_t *fs);
int exfat_commit_queue(exfat_fs_t *fs);
int exfat_commit(exfat_fs_t *fs);

/* misc.c */
unsigned exfat_ilog2(unsigned long v);
unsigned long exfat_strlen(const char *s);
int exfat_streq(const char *a, const char *b);
uint32_t exfat_boot_checksum(const uint8_t *region, unsigned long ss);
uint16_t exfat_set_checksum(const uint8_t *set, unsigned entries);
uint16_t exfat_name_hash(const uint16_t *up, unsigned len);
uint32_t exfat_table_checksum(uint32_t sum, const uint8_t *p,
			      unsigned long n);
void exfat_rwsem_init(exfat_rwsem_t *s, const char *name);
void exfat_rwsem_read_lock(exfat_rwsem_t *s);
void exfat_rwsem_read_unlock(exfat_rwsem_t *s);
void exfat_rwsem_write_lock(exfat_rwsem_t *s);
void exfat_rwsem_write_unlock(exfat_rwsem_t *s);
int exfat_rwsem_held_excl(exfat_rwsem_t *s);
int exfat_rwsem_release_owner(exfat_rwsem_t *s, uint64_t task_id);
void exfat_slock_shared(exfat_fs_t *fs, uint32_t h);
void exfat_sunlock_shared(exfat_fs_t *fs, uint32_t h);
void exfat_slock_excl(exfat_fs_t *fs, uint32_t h);
void exfat_sunlock_excl(exfat_fs_t *fs, uint32_t h);
int exfat_dev_read(exfat_fs_t *fs, uint64_t lba, uint64_t count,
		   void *buf);
uint64_t exfat_boot_dev_sectors(exfat_fs_t *fs);
int exfat_dev_write(exfat_fs_t *fs, uint64_t lba, uint64_t count,
		    const void *buf, int kind);
int exfat_dev_discard(exfat_fs_t *fs, uint64_t lba, uint64_t count);
int exfat_dev_sync(exfat_fs_t *fs);
void exfat_guard_init(exfat_fs_t *fs);
int exfat_dev_zero(exfat_fs_t *fs, uint64_t lba, uint64_t count);
const char *exfat_name_of(exfat_fs_t *fs);
void exfat_fs_error(exfat_fs_t *fs, const char *what, uint32_t where);
void exfat_time_encode(exfat_fs_t *fs, int64_t sec, long nsec,
		       uint32_t *ts, uint8_t *cs10, uint8_t *utc);
void exfat_time_decode(exfat_fs_t *fs, uint32_t ts, uint8_t cs10,
		       uint8_t utc, uint64_t *sec, uint64_t *nsec);
void exfat_now(exfat_fs_t *fs, uint32_t *ts, uint8_t *cs10,
	       uint8_t *utc);

/* cache.c */
uint64_t exfat_mc_region_bytes(exfat_fs_t *fs, int region);
exfat_mcu_t *exfat_mc_get(exfat_fs_t *fs, int region, uint32_t unit,
			  int *err);
void exfat_mc_put(exfat_fs_t *fs, exfat_mcu_t *u);
void exfat_mc_dirty(exfat_fs_t *fs, exfat_mcu_t *u, unsigned off,
		    unsigned len);
int exfat_mc_flush_region(exfat_fs_t *fs, int region);
int exfat_mc_flush(exfat_fs_t *fs);
void exfat_mc_destroy(exfat_fs_t *fs);

/* fatent.c */
int exfat_chain_decode(exfat_fs_t *fs, uint32_t first, int nofat,
		       uint64_t want, uint64_t max, exfat_extent_t **ext,
		       uint32_t *next, uint32_t *cap, uint32_t *ncl);
uint32_t exfat_ext_lookup(const exfat_extent_t *ext, uint32_t n,
			  uint64_t ci);
int exfat_stream_append(exfat_fs_t *fs, exfat_stream_t *st,
			const struct exfat_run *runs, unsigned nruns,
			int *pinned);
void exfat_runs_unalloc(exfat_fs_t *fs, const struct exfat_run *runs,
			unsigned nruns, uint32_t total, int pinned);
int exfat_stream_grow(exfat_fs_t *fs, exfat_stream_t *st, uint32_t ncl);
int exfat_shrink_prepare(exfat_fs_t *fs, exfat_stream_t *st,
			 uint32_t ncl, exfat_extent_t **tail,
			 uint32_t *ntail);
int exfat_shrink_apply(exfat_fs_t *fs, exfat_stream_t *st,
		       uint32_t ncl, exfat_extent_t *tail,
		       uint32_t ntail);
int exfat_stream_shrink(exfat_fs_t *fs, exfat_stream_t *st, uint32_t ncl);
int exfat_secondary_allocs(exfat_fs_t *fs, const uint8_t *set,
			   exfat_extent_t **out, uint32_t *nout);

/* balloc.c */
int exfat_bm_test(exfat_fs_t *fs, uint32_t cl);
int exfat_bm_change(exfat_fs_t *fs, uint32_t start, uint32_t len,
		    int value);
int exfat_bm_count_free(exfat_fs_t *fs);
void exfat_free_count_add(exfat_fs_t *fs, int64_t delta);
int exfat_alloc(exfat_fs_t *fs, uint32_t last, uint32_t n,
		struct exfat_run **runs, unsigned *nruns);
int exfat_free_extents(exfat_fs_t *fs, const exfat_extent_t *ext,
		       uint32_t n);
int exfat_free_reserve(exfat_fs_t *fs, uint64_t runs, unsigned fats);

/* nls.c */
void exfat_upcase_builtin(uint16_t *t);
int exfat_upcase_expand(const uint8_t *raw, unsigned long n,
			uint16_t *t);
int exfat_utf16_to_utf8(const uint16_t *in, unsigned n, char *out,
			unsigned cap);
int exfat_name_prepare(exfat_fs_t *fs, exfat_ctx_t *ctx,
		       const char *comp, unsigned long len,
		       int for_create, uint16_t *hash);

/* inode.c */
extern const vfs_sb_ops_t exfat_sb_ops;
int exfat_handle_alloc(exfat_fs_t *fs, exfat_stream_t *st);
void exfat_khash_insert_locked(exfat_fs_t *fs, exfat_stream_t *st);
void exfat_stream_hold(exfat_stream_t *st);
void exfat_stream_free(exfat_stream_t *st);
void exfat_stream_put(exfat_stream_t *st);
void exfat_stream_maint(exfat_fs_t *fs);
int exfat_stream_decode(exfat_fs_t *fs, exfat_stream_t *st);
exfat_stream_t *exfat_stream_get(exfat_fs_t *fs, exfat_stream_t *parent,
				 const exfat_dentry_info_t *di,
				 int *err);
exfat_stream_t *exfat_stream_find(exfat_fs_t *fs, uint64_t key);
void exfat_stream_rekey(exfat_fs_t *fs, exfat_stream_t *st,
			exfat_stream_t *new_parent, uint32_t index,
			uint8_t secondary_count);
void exfat_stream_orphan(exfat_fs_t *fs, exfat_stream_t *st);
unsigned long exfat_nofill_page(exfat_fs_t *fs, const exfat_stream_t *st);
unsigned long exfat_st_block_lba(exfat_fs_t *fs, const exfat_stream_t *st,
				 unsigned long n);

/* dir.c */
void exfat_diter_init(exfat_diter_t *it, exfat_fs_t *fs,
		      exfat_stream_t *d);
void exfat_diter_done(exfat_diter_t *it);
const uint8_t *exfat_diter_get(exfat_diter_t *it, uint64_t idx);
unsigned exfat_skip_slots(const uint8_t *e);
int exfat_set_parse(exfat_fs_t *fs, const uint8_t *set,
		    exfat_dentry_info_t *di, uint16_t *name);
int exfat_set_read(exfat_diter_t *it, uint64_t idx, uint8_t *set,
		   exfat_dentry_info_t *di, uint16_t *name);
int exfat_name_eq(exfat_fs_t *fs, const uint16_t *a, const uint16_t *up,
		  unsigned n);
int exfat_dir_write(exfat_fs_t *fs, exfat_stream_t *d, uint64_t idx,
		    unsigned count, const uint8_t *buf);
int exfat_dir_clear(exfat_fs_t *fs, exfat_stream_t *d, uint64_t idx,
		    unsigned count, uint8_t *scratch);
int exfat_dirw_flush(exfat_fs_t *fs);
unsigned exfat_set_build(exfat_fs_t *fs, uint8_t *set,
			 const uint16_t *name, unsigned nlen,
			 uint16_t hash, uint16_t attr, uint8_t sflags,
			 uint32_t first, uint64_t size, uint64_t vdl);
unsigned exfat_set_rename(const uint8_t *old, const uint16_t *name,
			  unsigned nlen, uint16_t hash, uint8_t *out);
int exfat_slot_fits(exfat_fs_t *fs, exfat_stream_t *d, uint64_t idx,
		    unsigned count);
int exfat_dir_alloc_slots(exfat_fs_t *fs, exfat_stream_t *d,
			  unsigned need, uint64_t *idx);
int exfat_dir_empty(exfat_fs_t *fs, exfat_stream_t *d);
int exfat_stream_store(exfat_fs_t *fs, exfat_stream_t *st);
long exfat_readdir_op(vfs_file_t *f, void *ubuf, long ubytes);

/* namei.c */
void exfat_walk_release(struct exfat_walk *w);
int exfat_walk(exfat_fs_t *fs, exfat_ctx_t *ctx, const char *path,
	       struct exfat_walk *w);
int exfat_walk_obj(exfat_fs_t *fs, exfat_ctx_t *ctx, const char *path,
		   struct exfat_walk *w);
exfat_ctx_t *exfat_ctx_new(void);
void exfat_ctx_free(exfat_ctx_t *ctx);
int exfat_create_locked(exfat_fs_t *fs, exfat_ctx_t *ctx,
			struct exfat_walk *w, unsigned int mode,
			exfat_stream_t **out);
int exfat_unlink_op(vfs_mount_t *m, const char *path);
int exfat_rmdir_op(vfs_mount_t *m, const char *path);
int exfat_mkdir_op(vfs_mount_t *m, const char *path, unsigned int mode);
int exfat_rename_op(vfs_mount_t *m, const char *oldpath,
		    const char *newpath);
int exfat_chdir_op(vfs_mount_t *m, const char *path);

/* file.c */
void exfat_touch_mtime(exfat_fs_t *fs, exfat_stream_t *st);
exfat_fs_t *exfat_fs_of(vfs_mount_t *m);
uint64_t exfat_ino(exfat_fs_t *fs, uint32_t first, uint32_t dir_first,
		   uint32_t index);
uint64_t exfat_st_ino(exfat_fs_t *fs, const exfat_stream_t *st);
int exfat_stat_locked(exfat_fs_t *fs, exfat_ctx_t *ctx,
		      const char *path, struct kstat *k);
int exfat_stat_op(vfs_mount_t *m, const char *path, struct kstat *k);
int exfat_fstat_op(vfs_file_t *f, struct kstat *k);
int exfat_fence(exfat_fs_t *fs, exfat_ctx_t *ctx, const char *path,
		exfat_stream_t **stp);
void exfat_unfence(exfat_fs_t *fs, exfat_stream_t *st);
int exfat_open_op(vfs_mount_t *m, const char *path, int flags,
		  vfs_file_t **out);
int exfat_open_mode_op(vfs_mount_t *m, const char *path, int flags,
		       unsigned int mode, vfs_file_t **out);
int exfat_close_op(vfs_file_t *f);
long exfat_read_op(vfs_file_t *f, void *buf, long bytes);
long exfat_read_at_op(vfs_file_t *f, void *buf, long bytes, long off);
long exfat_write_op(vfs_file_t *f, const void *buf, long bytes);
long exfat_write_at_op(vfs_file_t *f, const void *buf, long bytes,
		       long off);
long exfat_seek_op(vfs_file_t *f, long offset, int whence);
int exfat_truncate_op(vfs_file_t *f, unsigned long size);
int exfat_chmod_op(vfs_mount_t *m, const char *path, unsigned int mode);
int exfat_fchmod_op(vfs_file_t *f, unsigned int mode);
int exfat_utimens_op(vfs_mount_t *m, const char *path, int nofollow,
		     const struct vfs_utimes *t);
int exfat_futimens_op(vfs_file_t *f, const struct vfs_utimes *t);
int exfat_fsync_op(vfs_file_t *f);

/* ===========================================================================
 * Inline helpers shared between the files
 * =========================================================================== */

static inline uint16_t exfat_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t exfat_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline uint64_t exfat_le64(const uint8_t *p)
{
	return (uint64_t)exfat_le32(p) | ((uint64_t)exfat_le32(p + 4) << 32);
}

static inline void exfat_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void exfat_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline void exfat_put64(uint8_t *p, uint64_t v)
{
	exfat_put32(p, (uint32_t)v);
	exfat_put32(p + 4, (uint32_t)(v >> 32));
}

static inline uint64_t exfat_min64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

static inline uint64_t exfat_max64(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

/* a + b without wrapping: 0 when it would. */
static inline int exfat_add_ok(uint64_t a, uint64_t b, uint64_t *out)
{
	if (a + b < a)
		return 0;
	*out = a + b;
	return 1;
}

static inline uint64_t exfat_round_up(uint64_t v, uint64_t align)
{
	return (v + align - 1) & ~(align - 1);
}

static inline int exfat_is_pow2(unsigned long v)
{
	return v && !(v & (v - 1));
}

static inline uint64_t exfat_task_id(void)
{
	task_t *cur = sched_current();

	return cur ? cur->id : 0;
}

static inline void exfat_meta_rlock(exfat_fs_t *fs)
{
	exfat_rwsem_read_lock(&fs->meta);
}

static inline void exfat_meta_runlock(exfat_fs_t *fs)
{
	exfat_rwsem_read_unlock(&fs->meta);
}

static inline void exfat_meta_wlock(exfat_fs_t *fs)
{
	exfat_rwsem_write_lock(&fs->meta);
}

/* The end of an exclusive hold of a filesystem operation: parked orphans
 * are destroyed and the stream LRU trimmed while it is still held (the
 * page cache's writeback unlocks without this, see exfat_sb_unlock_io). */
static inline void exfat_meta_wunlock(exfat_fs_t *fs)
{
	if (fs->meta.wdepth == 1 && fs->meta.owner == exfat_task_id() &&
	    (fs->reap_head || fs->lru_count > EXFAT_STREAM_LRU_MAX))
		exfat_stream_maint(fs);
	exfat_rwsem_write_unlock(&fs->meta);
}

static inline void exfat_alloc_lock(exfat_fs_t *fs)
{
	exfat_assert_excl(fs);
	exfat_rwsem_write_lock(&fs->alloc);
}

static inline void exfat_alloc_unlock(exfat_fs_t *fs)
{
	exfat_rwsem_write_unlock(&fs->alloc);
}

/* May the instance be modified now?  ST_OK or ST_ROFS. */
static inline int exfat_writable(exfat_fs_t *fs)
{
	if (fs->ro || fs->ro_latched)
		return ST_ROFS;
	return ST_OK;
}

static inline int exfat_cluster_ok(exfat_fs_t *fs, uint64_t cl)
{
	return cl >= EXFAT_FIRST_CLUSTER &&
	       cl <= (uint64_t)fs->cluster_count + 1;
}

static inline uint16_t exfat_upc(exfat_fs_t *fs, uint16_t c)
{
	return fs->upcase[c];
}

static inline unsigned long exfat_chain_of(const exfat_stream_t *st)
{
	return EXFAT_BID_ENC(st->handle, 0);
}

/* The disk cluster of stream cluster `ci', 0 beyond the allocation. */
static inline uint32_t exfat_st_cluster(const exfat_stream_t *st, uint64_t ci)
{
	if (ci >= st->nclusters)
		return 0;
	return exfat_ext_lookup(st->ext, st->next, ci);
}

/* The last disk cluster of the stream, 0 when it has none. */
static inline uint32_t exfat_st_last(const exfat_stream_t *st)
{
	if (!st->next)
		return 0;
	return st->ext[st->next - 1].disk + st->ext[st->next - 1].len - 1;
}

/* Number of entry slots of a directory. */
static inline uint64_t exfat_dir_slots(const exfat_stream_t *d)
{
	return d->size >> EXFAT_DENTRY_SHIFT;
}

/* Is `type' a primary entry without a SecondaryCount field? */
static inline int exfat_primary_single(uint8_t type)
{
	return type == EXFAT_ET_BITMAP || type == EXFAT_ET_UPCASE ||
	       type == EXFAT_ET_LABEL;
}

/* Note page `pidx' of `st' as dirtied (the range the commit checks). */
static inline void exfat_note_dirty(exfat_stream_t *st, unsigned long pidx)
{
	if (st->dhi < st->dlo) {
		st->dlo = st->dhi = pidx;
		return;
	}
	if (pidx < st->dlo)
		st->dlo = pidx;
	if (pidx > st->dhi)
		st->dhi = pidx;
}

#endif /* LIKEOS_EXFAT_INT_H */
