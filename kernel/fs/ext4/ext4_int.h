// LikeOS - ext4 filesystem driver: declarations shared by the files of the
// driver (kernel/fs/ext4/); not for use outside it.
//
// A full read/write ext4 driver: extent + indirect block mapping, directory
// traversal, allocation and metadata writeback, symlinks/hard links, journaling
// (ordered mode), metadata_csum (crc32c), xattrs/ACLs, and permission
// enforcement, integrated with the generic page/inode caches.
// Structurally this mirrors the FAT driver (kernel/fs/fat32/): a
// reentrant sleeping I/O mutex serialises operations, the data path runs
// through the shared pagecache, and the two vtables (vfs_ops_t for path/
// handle ops, vfs_sb_ops_t for the cache's block plumbing) are filled in.
//
// The block-chain integration uses self-describing block ids — see the
// big comment in kernel/fs/ext4/ext4.h.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_EXT4_INT_H
#define LIKEOS_EXT4_INT_H

#include <kernel/fs/ext4/ext4.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/stat.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/mount.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/uapi/bug.h>
#include <kernel/mm/vmscan.h>
#include <kernel/dev/block/blk_internal.h>

/* ext4_fs_t.mnt_opts: what a mount(2) asked for beyond its flags. */
/* The journal is not replayed ("norecovery", "noload"); read-only only. */
#define EXT4_MNT_NOLOAD 0x1u

/* A journal that needs replaying on a write-protected device refuses the
 * mount instead of being skipped silently (every mount(2); the root at boot
 * keeps going as it always did). */
#define EXT4_MNT_WP_STRICT 0x2u

/* "recovery=ask": a journal that needs replaying refuses the mount with
 * ST_UCLEAN instead of being replayed; the caller asks the user and mounts
 * again without the option to replay it. */
#define EXT4_MNT_ASK_REPLAY 0x4u

/* Max sectors per device transfer (== USB_MSD_MAX_BLOCKS in usb_msd.c).  Also the
 * size of the write-back coalescing buffer (st->wbounce).  2048 sectors = 1 MiB:
 * large transfers amortise the USB per-command latency (the throughput limiter),
 * and the xHCI ring (256 TRBs) chains the needed 16 TRBs comfortably. */
#define EXT4_MAX_SECTORS_PER_READ 2048

#define EXT4_INO_CSUM_LO_OFF 0x7C /* l_i_checksum_lo within i_osd2[8]   */

#define EXT4_INO_CSUM_HI_OFF 0x82 /* i_checksum_hi                      */

#define EXT4_GOOD_OLD_ISIZE 128

#define EXT4_DIR_TAIL_SIZE 12

/* bg_flags bits: an uninitialized group's bitmap is not laid out on disk yet, so
 * its stored csum is over the would-be-initialized image — skip verification. */
#define EXT4_BG_INODE_UNINIT 0x0001

#define EXT4_BG_BLOCK_UNINIT 0x0002

typedef struct {
	volatile int readers; /* active shared holders                  */
	volatile int writer; /* 1 while exclusive is held              */
	volatile uint64_t owner; /* exclusive owner task id                */
	volatile int wdepth; /* exclusive recursion depth              */
	volatile int w_wait; /* writers queued (fairness hint)         */
	/* The other half of fairness.  Deferring every new reader to any
	 * queued writer keeps readers from starving mutators -- but with
	 * nothing bounding the OTHER direction, a continuous stream of
	 * mutators (each queuing while its predecessor holds) kept w_wait
	 * nonzero for as long as the stream lasted, and a first-time reader
	 * never got a turn at all.  Measured: an exec's path resolution
	 * starved for over two seconds behind another process's file-creation
	 * storm, so its tracer concluded the exec stop was never coming.
	 *
	 * So the sem alternates in phases: when an exclusive release finds
	 * readers waiting, THAT cohort (r_wait of them) is admitted before
	 * the next writer -- reader_turn stays set until the cohort has
	 * drained, and writers queue behind it.  Readers arriving during the
	 * cohort's turn wait for the next phase, so the alternation is
	 * bounded in both directions: a reader waits out at most one
	 * exclusive hold, a writer at most one cohort. */
	volatile int r_wait; /* readers queued (cohort size)           */
	volatile int reader_turn; /* queued cohort admitted before writers  */
	/* Shared holds, attributed to their tasks.  A task can die inside
	 * the driver only one way: a fault in kernel mode on a user address
	 * abandons its frame (the scheduler never ends another task in place;
	 * one asleep here is woken by a fatal signal and unwinds normally).
	 * The driver touches user memory with no lock held, and on top of
	 * that every shared acquisition outside an exclusive hold is recorded
	 * per task in rh[0..rh_n) -- holds beyond the table are counted in
	 * `untracked' -- so release_locks_for_task can give a dead task's
	 * shared holds back as well as its exclusive one. */
	struct ext4_rhold *rh; /* rh_n slots, or none           */
	unsigned rh_n; /* slots in rh                             */
	unsigned rh_hi; /* slots [0, rh_hi) may be in use     */
	int untracked; /* shared holds not in rh                  */
	spinlock_t lock; /* protects the fields above              */
} ext4_rwsem_t;

/* One task's shared holds of an ext4_rwsem_t. */
typedef struct ext4_rhold {
	uint64_t task; /* holder's task id                 */
	int count; /* its shared holds; 0: slot free   */
} ext4_rhold_t;

#define EXT4_RWSEM_INIT(name)                                        \
	{                                                            \
		.readers = 0, .writer = 0, .owner = (uint64_t)-1,    \
		.wdepth = 0, .w_wait = 0, .r_wait = 0,               \
		.reader_turn = 0, .rh = 0, .rh_n = 0, .rh_hi = 0,    \
		.untracked = 0, .lock = SPINLOCK_INIT(name)          \
	}

/* Tasks whose shared holds are recorded at a time: of the metadata lock
 * (every reader of the filesystem takes it), of each per-inode lock. */
#define EXT4_META_TRACK 64

#define EXT4_ILOCK_TRACK 8

#define EXT4_ILOCKS 64

#define EXT4_BGLOCKS 32

/* Metadata block cache: buckets of the block-number hash.  The cache itself
 * has no fixed size -- see "Metadata block cache" in buffer.c. */
#define EXT4_MBC_HASH 16384

/* One cached metadata block. */
struct ext4_mbc_ent {
	unsigned long pbn;
	uint8_t *data; /* `size' bytes                                  */
	unsigned size; /* allocated bytes (buffer-cache accounting)     */
	int verified; /* type-specific csum already checked once       */
	int page; /* data is a whole page of its own (not kalloc)  */
	struct ext4_mbc_ent *hnext; /* hash chain                      */
	struct ext4_mbc_ent *lru_prev, *lru_next; /* most recent first */
};

#define EXT4_BPOOL 16

#define EXT4_DI_MAX 64

#define EXT4_IC_ENTRIES 8

/* One run of metadata blocks [start, start+len); `ino': the inode whose
 * blocks they are (the journal), 0 for the metadata of the block groups. */
struct ext4_sysz {
	unsigned long start, len;
	unsigned long ino;
};

struct ext4_state {
	/* Journaled-writes transaction of the current top-level operation
	 * (see "Journaled-writes transaction" below). */
	struct {
		int active;
		unsigned n, cap;
		unsigned long *blk; /* final physical block number of each entry   */
		uint8_t **data; /* block_size bytes each (reused across txns)   */
	} txn;
	/* Committed metadata not yet checkpointed (the batch). */
	struct {
		unsigned n, cap;
		unsigned long *blk; /* final physical block number of each entry   */
		uint8_t **data; /* block_size bytes each                        */
	} ckpt;
	unsigned long jhead; /* next free journal log block        */
	int j_upgrade; /* journal to become csum v3 with the first commit */
	/* Blocks freed by operations not yet committed to the journal (see
	 * ext4_pf_add): runs [start, start+len).  Entries [0, pf_merged)
	 * belong to operations already merged into the batch (st->ckpt);
	 * the rest to the running one. */
	struct ext4_pf_run {
		unsigned long start, len;
	} *pf;
	unsigned pf_n, pf_cap, pf_merged;
	int epoch_open; /* committed, un-checkpointed txns?    */
	uint32_t epoch_seq; /* sequence of the epoch's first txn   */

	/* The lock hierarchy (see "Fine-grained locking engine"). */
	ext4_rwsem_t meta;
	ext4_rwsem_t ilock[EXT4_ILOCKS];
	ext4_rwsem_t bglock[EXT4_BGLOCKS]; /* exclusive only: untracked */
	ext4_rhold_t meta_rh[EXT4_META_TRACK];
	ext4_rhold_t ilock_rh[EXT4_ILOCKS][EXT4_ILOCK_TRACK];

	/* Metadata block cache (buffer.c). */
	struct ext4_mbc_ent **mbc_hash; /* EXT4_MBC_HASH chains             */
	struct ext4_mbc_ent mbc_lru; /* sentinel: most recent at lru_next  */
	unsigned long mbc_count;
	spinlock_t mbc_lock;
	struct vm_shrinker mbc_shrinker; /* gives blocks back under pressure */

	/* Recycled scratch block buffers. */
	struct {
		uint8_t *data;
		unsigned size;
		int busy;
	} bpool[EXT4_BPOOL];
	spinlock_t bpool_lock;

	/* Deferred size/time updates. */
	struct {
		unsigned long ino; /* 0 = empty */
		uint64_t size;
		uint32_t mtime;
		uint32_t ctime;
	} di[EXT4_DI_MAX];
	unsigned di_n;
	spinlock_t di_lock;

	/* Parsed-inode cache. */
	struct {
		unsigned long ino; /* 0 = empty */
		ext4_inode inode;
	} ic[EXT4_IC_ENTRIES];
	unsigned ic_next;
	spinlock_t ic_lock;

	int flush_unsupported; /* device refused the mount-time probe */

	/* Errors found on the filesystem (ext4_fs_error).  The messages are
	 * rate limited: a window opened at err_rl_begin (timer ticks), the
	 * messages printed in it and those held back.  err_stored: the
	 * superblock was written with this mount's error state; err_dirty:
	 * it has error details since then that are not stored yet (they are
	 * at unmount or sync). */
	spinlock_t err_lock;
	uint64_t err_rl_begin;
	unsigned err_rl_printed, err_rl_missed;
	int err_stored;
	int err_dirty;
	/* The journal was aborted by an error: nothing more is committed to
	 * it or checkpointed from it (ext4_journal_abort). */
	int j_aborted;

	/* Group descriptors touched since the last write-out. */
	uint8_t *gd_dirty; /* 1 byte/group: this group changed       */
	unsigned gd_dirty_cap; /* allocated length of gd_dirty            */
	unsigned gd_lo, gd_hi; /* dirty-group span (lo>hi = empty) */
	unsigned gd_seen_lo, gd_seen_hi; /* groups touched since mount (never reset) */

	uint8_t *wbounce; /* write coalescing buffer (allocated at mount) */
	unsigned long wbounce_bytes;
	uint8_t *gd_buf; /* group-descriptor scratch block */

	/* The data write-back buffer's run. */
	unsigned long wb_pbn; /* physical start block of the buffered run */
	unsigned wb_len; /* blocks currently buffered (0 = empty)    */
	unsigned long wb_ino; /* inode the buffered data belongs to       */
	int wb_err; /* deferred flush error, surfaced at fsync  */

	/* Inodes with data the current batch must store first. */
	unsigned long *ord_ino;
	unsigned ord_n;
	unsigned ord_cap;
	int ord_overflow;
	spinlock_t ord_lock;

	/* The system zone: the blocks holding the filesystem's own metadata,
	 * as sorted runs that neither overlap nor touch with the same owner
	 * (see ext4_setup_system_zone).  Built at mount, read-only after. */
	struct ext4_sysz *sysz;
	unsigned sysz_n;

	/* Directory a relative path starts from (paths arrive absolute). */
	unsigned long cwd_ino;
	/* This filesystem is the root (the scheduler's debug dump mirrors its
	 * lock state). */
	int is_root;
	/* The periodic commit thread's list of filesystems, and how many of
	 * its passes are working on this one right now (an unmount waits for
	 * them; see ext4_flush_thread). */
	ext4_fs_t *commit_next;
	int commit_busy;
};

#define EXT4_T_ATIME 1u

#define EXT4_T_MTIME 2u

#define EXT4_T_CTIME 4u

/* Refuse a mutating op up front when the fs is read-only / error-latched.  The
 * block-write chokepoints (ext4_write_block_direct / ext4_write_impl) already
 * prevent corruption; this just returns a clean -EROFS at entry instead of
 * letting an op do partial work and fail partway. */
static inline int ext4_is_ro(ext4_fs_t *fs)
{
	return fs && fs->read_only;
}

#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200

/* The read-only-compatible features writes keep intact. */
#define EXT4_RO_COMPAT_WRITABLE                                               \
	(EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |                                \
	 EXT4_FEATURE_RO_COMPAT_LARGE_FILE | EXT4_FEATURE_RO_COMPAT_HUGE_FILE | \
	 EXT4_FEATURE_RO_COMPAT_GDT_CSUM | EXT4_FEATURE_RO_COMPAT_DIR_NLINK |  \
	 EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |                                  \
	 EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)

static inline void ext4_inode_cache_drop(ext4_fs_t *fs, unsigned long ino)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	for (int i = 0; i < EXT4_IC_ENTRIES; i++)
		if (fs->st->ic[i].ino == ino)
			fs->st->ic[i].ino = 0;
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
}

static inline unsigned long ext4_inode_size(const ext4_inode *in)
{
	/* For regular files i_size_high holds the high 32 bits; for directories
     * the field is i_dir_acl and the directory size fits in 32 bits. */
	if ((in->i_mode & S_IFMT) == S_IFREG)
		return (unsigned long)in->i_size_lo |
		       ((unsigned long)in->i_size_high << 32);
	return in->i_size_lo;
}

static inline int ext4_bm_test(const uint8_t *bm, unsigned long b)
{
	return (bm[b >> 3] >> (b & 7)) & 1;
}

static inline void ext4_bm_set(uint8_t *bm, unsigned long b)
{
	bm[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static inline void ext4_bm_clear(uint8_t *bm, unsigned long b)
{
	bm[b >> 3] &= (uint8_t) ~(1u << (b & 7));
}

/* Status returned by the bitmap loaders when the group's metadata is
 * corrupt (the error has already been recorded through ext4_fs_error); the
 * allocators stop instead of trying the next group. */
#define EXT4_BM_CORRUPT ST_INVALID

#define EXT4_HASH_LEGACY 0

#define EXT4_HASH_HALF_MD4 1

#define EXT4_HASH_TEA 2

#define EXT4_HASH_LEGACY_UNS 3

#define EXT4_HASH_HALF_MD4_UNS 4

#define EXT4_HASH_TEA_UNS 5

#define EXT4_HTREE_EOF 0x7fffffffUL

typedef struct {
	uint32_t reserved_zero;
	uint8_t hash_version;
	uint8_t info_length;
	uint8_t indirect_levels;
	uint8_t unused_flags;
} __attribute__((packed)) ext4_dx_root_info;

typedef struct {
	uint32_t hash;
	uint32_t block;
} __attribute__((packed)) ext4_dx_entry;

typedef struct {
	uint16_t limit;
	uint16_t count;
} __attribute__((packed)) ext4_dx_countlimit;

typedef struct {
	uint32_t dt_reserved;
	uint32_t dt_checksum;
} __attribute__((packed)) ext4_dx_tail;

/* The instance a VFS call is for. */
static inline ext4_fs_t *ext4_fs_of(vfs_mount_t *m)
{
	return m ? (ext4_fs_t *)m->fs_data : NULL;
}

static inline uint32_t be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

/* ---- super.c ----------------------------------------------------------- */

/* Record a metadata-corruption error + apply the errors=
 * policy (remount-ro latch with the journal aborted / panic / continue).
 * The message is rate limited; the function and line calling it are kept
 * in the superblock's error record. */
void __ext4_fs_error(ext4_fs_t *fs, const char *what, unsigned long ino,
		     const char *func, unsigned line);
#define ext4_fs_error(fs, what, ino) \
	__ext4_fs_error((fs), (what), (ino), __func__, __LINE__)

/* A block number taken from the disk (`what': from where) that inode `ino'
 * may not use, see ext4_blk_valid: said, and recorded via ext4_fs_error. */
struct ext4_err_site {
	const char *func;
	unsigned line;
};
void __ext4_bad_block(ext4_fs_t *fs, unsigned long ino, const char *what,
		      unsigned long pbn, unsigned long count,
		      const struct ext4_err_site *site);
#define ext4_bad_block(fs, ino, what, pbn, count)                          \
	do {                                                               \
		static const struct ext4_err_site __ext4_site = { __func__, \
								 __LINE__ }; \
		__ext4_bad_block((fs), (ino), (what), (pbn), (count),       \
				 &__ext4_site);                             \
	} while (0)

/* Store error details recorded since the superblock was last written
 * because of an error (unmount). */
void ext4_store_error_info(ext4_fs_t *fs);

/* The blocks the filesystem's own structures take (ext4_fs_t
 * overhead_blocks), from the layout; called once at mount. */
unsigned long ext4_calc_overhead(ext4_fs_t *fs);

int ext4_get_statfs(ext4_fs_t *fs, unsigned long *f_bsize, unsigned long *f_blocks,
		    unsigned long *f_bfree, unsigned long *f_bavail, unsigned long *f_files,
		    unsigned long *f_ffree, unsigned long *f_namelen,
		    unsigned long *f_type);

/* ---- csum.c ------------------------------------------------------------ */

uint32_t ext4_crc32c(uint32_t crc, const void *buf, unsigned long len);

uint32_t ext4_sb_csum(const ext4_super_block *sb);

uint16_t ext4_gd_csum(const ext4_fs_t *fs, uint32_t group,
			     const ext4_group_desc *gd);

uint32_t ext4_inode_csum(const ext4_fs_t *fs, unsigned long ino,
				const uint8_t *raw);

uint32_t ext4_inode_csum_seed(const ext4_fs_t *fs, unsigned long ino,
				     uint32_t gen);

void ext4_inode_csum_set(const ext4_fs_t *fs, unsigned long ino,
				uint8_t *raw);

void ext4_dir_csum_set(const ext4_fs_t *fs, unsigned long dir_ino,
			      uint32_t gen, uint8_t *blk);

void ext4_block_bitmap_csum_set(ext4_fs_t *fs, unsigned g,
				       const uint8_t *bm);

void ext4_inode_bitmap_csum_set(ext4_fs_t *fs, unsigned g,
				       const uint8_t *bm);

int ext4_dir_csum_ok(const ext4_fs_t *fs, unsigned long dir_ino,
			    uint32_t gen, const uint8_t *blk);

int ext4_block_bitmap_csum_match(const ext4_fs_t *fs, unsigned g,
					const uint8_t *bm);

int ext4_inode_bitmap_csum_match(const ext4_fs_t *fs, unsigned g,
					const uint8_t *bm);

int ext4_block_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g,
				     const uint8_t *bm);

int ext4_inode_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g,
				     const uint8_t *bm);

int ext4_extent_block_csum_ok(const ext4_fs_t *fs, unsigned long ino,
				     uint32_t gen, const uint8_t *blk);

int ext4_memcmp(const void *a, const void *b, unsigned long n);

/* ---- lock.c ------------------------------------------------------------ */

int ext4_rwsem_release_owner(ext4_rwsem_t *sem, uint64_t task_id,
				    int *had_excl);

void ext4_rwsem_write_lock(ext4_rwsem_t *sem);

void ext4_rwsem_write_unlock(ext4_rwsem_t *sem);

void ext4_rwsem_init(ext4_rwsem_t *sem, const char *name,
			    ext4_rhold_t *rh, unsigned rh_n);

extern volatile int ext4_io_locked;

extern volatile int ext4_io_depth;

extern volatile uint64_t ext4_io_owner;

void ext4_io_lock(ext4_fs_t *fs);

void ext4_io_unlock(ext4_fs_t *fs);

void ext4_meta_rlock(ext4_fs_t *fs);

void ext4_meta_runlock(ext4_fs_t *fs);

int ext4_meta_held_excl(ext4_fs_t *fs);

void ext4_ilock_shared(ext4_fs_t *fs, unsigned long ino);
pc_page_t *ext4_cache_page_impl(vfs_file_t *f, unsigned long idx);

void ext4_iunlock_shared(ext4_fs_t *fs, unsigned long ino);

void ext4_ilock_excl(ext4_fs_t *fs, unsigned long ino);

void ext4_iunlock_excl(ext4_fs_t *fs, unsigned long ino);

void ext4_bg_lock(ext4_fs_t *fs, unsigned g);

void ext4_bg_unlock(ext4_fs_t *fs, unsigned g);

/* ---- buffer.c ---------------------------------------------------------- */

int ext4_read_sectors(const block_device_t *bdev, unsigned long lba,
			     unsigned long count, void *buf);

int ext4_sb_read_raw(const block_device_t *bdev, unsigned long part_lba,
			    void *out);

int ext4_mbc_verified(ext4_fs_t *fs, unsigned long pbn);

void ext4_mbc_mark_verified(ext4_fs_t *fs, unsigned long pbn);

void ext4_mbc_invalidate(ext4_fs_t *fs);

void ext4_mbc_drop(ext4_fs_t *fs, unsigned long pbn);
/* Set up / tear down the cache of a filesystem's state (super.c). */
int ext4_mbc_init(ext4_fs_t *fs);
void ext4_mbc_destroy(ext4_fs_t *fs);
/* Refresh the cached copy of a block just written to the device. */
void ext4_mbc_written(ext4_fs_t *fs, unsigned long pbn, const void *buf);

int ext4_read_block(ext4_fs_t *fs, unsigned long pbn, void *buf);

void *ext4_bget(ext4_fs_t *fs);

void ext4_bput(ext4_fs_t *fs, void *p);

int ext4_write_sectors(const block_device_t *bdev, unsigned long lba,
			      unsigned long count, const void *buf);

int ext4_write_block_direct(ext4_fs_t *fs, unsigned long pbn,
				   const void *buf);

int ext4_write_block(ext4_fs_t *fs, unsigned long pbn, const void *buf);

int ext4_write_super(ext4_fs_t *fs);

int ext4_dev_sync(ext4_fs_t *fs, const char *where);

void ext4_probe_dev_sync(ext4_fs_t *fs);

/* Is [pbn, pbn+count) inside the filesystem?  Else reported, and 0: the
 * last check of every write (buffer.c). */
int ext4_write_target_ok(ext4_fs_t *fs, unsigned long pbn, unsigned long count);

/* ---- balloc.c ---------------------------------------------------------- */

int ext4_uninit_honoured(const ext4_fs_t *fs);

unsigned long ext4_group_first_block(const ext4_fs_t *fs, unsigned g);

unsigned ext4_blocks_in_group(const ext4_fs_t *fs, unsigned g);

unsigned ext4_gdt_blocks(const ext4_fs_t *fs);

unsigned ext4_itable_blocks(const ext4_fs_t *fs);

int ext4_read_group_desc(ext4_fs_t *fs, unsigned int group,
				ext4_group_desc *out);

unsigned long ext4_inode_table_block(ext4_fs_t *fs,
					    const ext4_group_desc *gd);

int ext4_wb_flush(ext4_fs_t *fs); /* flush the data write-back buffer */

void ext4_gd_dirty(ext4_fs_t *fs, unsigned g);

void ext4_flush_meta(ext4_fs_t *fs);

unsigned long ext4_gd_block_bitmap(ext4_fs_t *fs, unsigned g);

unsigned long ext4_gd_inode_bitmap(ext4_fs_t *fs, unsigned g);

void ext4_mark_bitmap_end(unsigned start, unsigned end, uint8_t *bm);

int ext4_block_in_group(const ext4_fs_t *fs, unsigned long b, unsigned g);

int ext4_init_block_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm);

unsigned long ext4_bitmap_free_blocks(const ext4_fs_t *fs, unsigned g,
					     const uint8_t *bm);

int ext4_load_block_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm,
				  int *uninit);

int ext4_load_inode_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm,
				  int *uninit);

void ext4_bg_block_uninit_clear(ext4_fs_t *fs, unsigned g,
				       unsigned long init_free);

int ext4_bg_block_bitmap_materialize(ext4_fs_t *fs, unsigned g);

/* Self-heal at sync (defined after the bitmap helpers below). */
void ext4_resync_gd_from_bitmaps(ext4_fs_t *fs);

/* Is [pbn, pbn+count) a run of blocks inode `ino' may use (0: an inode
 * that owns no metadata)?  Inside the filesystem and outside the system
 * zone, except for the runs `ino' owns there.  1 or 0. */
int ext4_blk_valid(const ext4_fs_t *fs, unsigned long ino, unsigned long pbn,
		   unsigned long count);

/* Every group's bitmaps and inode table inside the filesystem and clear of
 * the superblock and the descriptor table.  ST_OK or ST_INVALID (said). */
int ext4_check_descriptors(ext4_fs_t *fs);

/* (Re)build the system zone from the group descriptors plus `extra' (the
 * journal's runs).  ST_OK, ST_INVALID (overlapping metadata, said) or
 * ST_NOMEM. */
int ext4_setup_system_zone(ext4_fs_t *fs, const struct ext4_sysz *extra,
			   unsigned nextra);

/* ---- ialloc.c ---------------------------------------------------------- */

unsigned ext4_itable_unused(const ext4_fs_t *fs, unsigned g);

void ext4_itable_unused_set(ext4_fs_t *fs, unsigned g, unsigned v);

unsigned long ext4_alloc_inode(ext4_fs_t *fs, unsigned long parent_ino,
				      int is_dir);

void ext4_free_inode(ext4_fs_t *fs, unsigned long ino, int is_dir);

void ext4_init_owner(unsigned puid, unsigned pgid, unsigned pmode,
			    int is_dir, unsigned *uid, unsigned *gid,
			    unsigned *mode);

int ext4_write_inode_new(ext4_fs_t *fs, unsigned long ino,
				const ext4_inode *in);

void ext4_abandon_new_inode(ext4_fs_t *fs, unsigned long ino,
				   int is_dir);

int ext4_create_inode(ext4_fs_t *fs, unsigned long ino, unsigned mode,
			     unsigned uid, unsigned gid);

/* ---- mballoc.c --------------------------------------------------------- */

void ext4_pf_release(ext4_fs_t *fs, unsigned upto);

int ext4_pf_mask(ext4_fs_t *fs, unsigned g, uint8_t *scan);

int ext4_pf_commit_for_space(ext4_fs_t *fs);

void ext4_free_blocks_run(ext4_fs_t *fs, unsigned long start,
				 unsigned long len);

void ext4_free_block(ext4_fs_t *fs,
			    unsigned long pbn); /* defined below */

unsigned long ext4_alloc_one_block(ext4_fs_t *fs); /* defined below */

/* ---- inode.c ----------------------------------------------------------- */

int ext4_inode_has_extra(const ext4_fs_t *fs, const ext4_inode *in,
				unsigned off);

void ext4_time_decode(uint32_t lo, uint32_t extra, int has_extra,
			     uint64_t *sec, uint64_t *nsec);

void ext4_stamp(const ext4_fs_t *fs, ext4_inode *in, unsigned which,
		       int64_t sec, uint32_t nsec);

void ext4_stamp_now(const ext4_fs_t *fs, ext4_inode *in, unsigned which);

uint32_t ext4_inode_uid(const ext4_inode *in);

uint32_t ext4_inode_gid(const ext4_inode *in);

void ext4_inode_set_uid(ext4_inode *in, uint32_t uid);

void ext4_inode_set_gid(ext4_inode *in, uint32_t gid);

int ext4_di_record(ext4_fs_t *fs, unsigned long ino, uint64_t size, uint32_t mtime,
			  uint32_t ctime);

void ext4_di_clear(ext4_fs_t *fs, unsigned long ino);

int ext4_read_inode_loc(ext4_fs_t *fs, unsigned long ino,
			       ext4_inode *out, unsigned long *out_block,
			       unsigned *out_off);

void ext4_inode_cache_flush(ext4_fs_t *fs);

int ext4_get_inode_cached(ext4_fs_t *fs, unsigned long ino,
				 ext4_inode *out);

void ext4_inode_cache_update(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in);

/* Map (inode, logical block) to its physical block: ST_OK with *pbn the
 * block, or 0 for a hole / past the end; ST_IO when the block map is
 * corrupt (reported through ext4_fs_error) or cannot be read; ST_NOMEM. */
int ext4_map_blk(ext4_fs_t *fs, unsigned long ino, unsigned long lidx,
		 unsigned long *pbn);

/* The same from an inode image the caller holds. */
int ext4_map_in_blk(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in,
		    unsigned long lidx, unsigned long *pbn);

/* ext4_map_blk for callers that can use a block only: 0 for a hole and
 * for a failure alike. */
unsigned long ext4_block_map(ext4_fs_t *fs, unsigned long ino,
				    unsigned long lidx);

unsigned long ext4_map_in(ext4_fs_t *fs, unsigned long ino,
				 const ext4_inode *in, unsigned long lidx);

void ext4_sb_attach(ext4_fs_t *fs);

/* Persist a dirty cached inode's size back to disk (called by icache_flush).
 * Defined out-of-line below where the write helpers are available. */
int ext4_write_inode_struct(ext4_fs_t *fs, unsigned long ino,
				   const ext4_inode *in);

void ext4_di_flush(ext4_fs_t *fs);

unsigned ext4_alloc_blocks_for_file(ext4_fs_t *fs, unsigned long ino,
					   ext4_inode *in,
					   unsigned long start_lidx,
					   unsigned count);

void ext4_free_blocks_from(ext4_fs_t *fs, unsigned long ino,
				  ext4_inode *in, unsigned long from);

int ext4_truncate_impl(vfs_file_t *f, unsigned long size);

int ext4_utimens_ino(ext4_fs_t *fs, unsigned long ino,
			    const struct vfs_utimes *t);

int ext4_utimens_impl(ext4_fs_t *fs, const char *path, int nofollow,
			     const struct vfs_utimes *t);

int ext4_chmod(ext4_fs_t *fs, const char *path, unsigned mode);

int ext4_chown(ext4_fs_t *fs, const char *path, int uid, int gid,
		      int nofollow);

int ext4_fchmod_ino(ext4_fs_t *fs, unsigned long ino, unsigned mode);

int ext4_fchown_ino(ext4_fs_t *fs, unsigned long ino, int uid, int gid);

int ext4_lstat(ext4_fs_t *fs, const char *path, struct kstat *st);

/* ---- extents.c --------------------------------------------------------- */

int ext4_extent_map(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in,
		    unsigned long lidx, unsigned long *pbn);

unsigned ext4_ext_add(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			     unsigned long lidx, unsigned long pbn,
			     unsigned addlen);

unsigned ext4_ext_free_subtree(ext4_fs_t *fs, unsigned long ino,
				      uint32_t gen, uint8_t *node, int depth,
				      unsigned long from, unsigned long *freed);

/* Walking a block map: every mapped run of logical blocks below `nblocks'
 * is handed to fn (whose non-ST_OK status ends the walk), contiguous
 * blocks joined into one run.  Index and pointer blocks are checked on the
 * way (ext4_blk_valid for `ino'). */
struct ext4_walk {
	int (*fn)(ext4_fs_t *fs, void *arg, unsigned long lblk,
		  unsigned long pbn, unsigned long len);
	void *arg;
	unsigned long ino, nblocks;
	unsigned long lblk, pbn, len; /* the run being gathered */
};

int ext4_walk_emit(ext4_fs_t *fs, struct ext4_walk *w, unsigned long lblk,
		   unsigned long pbn, unsigned long len);

int ext4_ext_walk(ext4_fs_t *fs, struct ext4_walk *w, const ext4_inode *in);

/* ---- indirect.c -------------------------------------------------------- */

int ext4_indirect_map(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in,
		      unsigned long lidx, unsigned long *pbn);

int ext4_ind_walk(ext4_fs_t *fs, struct ext4_walk *w, const ext4_inode *in);

void ext4_new_block_map(const ext4_fs_t *fs, ext4_inode *in);

int ext4_ind_mapped(const ext4_inode *in);

int ext4_alloc_extents(const ext4_fs_t *fs, const ext4_inode *in);

unsigned ext4_ind_add(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			     unsigned long lidx, unsigned long pbn, unsigned len);

void ext4_ind_free_from(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			unsigned long from);

/* ---- namei.c ----------------------------------------------------------- */

void ext4_dc_drop_dir(ext4_fs_t *fs, unsigned long ino);

int ext4_dir_lookup(ext4_fs_t *fs, unsigned long dir_ino,
			   const char *name, unsigned name_len,
			   unsigned long *out_ino, unsigned *out_ftype);

int ext4_read_symlink_target(ext4_fs_t *fs, unsigned long ino,
				    const ext4_inode *in, char *buf,
				    unsigned cap);

int ext4_resolve_ex(ext4_fs_t *fs, unsigned long start_ino,
			   const char *path, int follow_final,
			   unsigned long *out_ino, int depth);

int ext4_resolve(ext4_fs_t *fs, const char *path, unsigned long *out_ino);

int ext4_dir_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long child_ino,
			unsigned ftype);

/* Write helpers (defined further below; used by open/truncate). */
int ext4_resolve_parent(ext4_fs_t *fs, const char *path,
			       unsigned long *parent_ino, char *name_out,
			       unsigned cap);

int ext4_unlink_impl(ext4_fs_t *fs, const char *path);

int ext4_mkdir_impl(ext4_fs_t *fs, const char *path, unsigned mode);

int ext4_mknod_impl(ext4_fs_t *fs, const char *path, unsigned mode,
			   uint64_t dev);

int ext4_rmdir_impl(ext4_fs_t *fs, const char *path);

int ext4_rename_impl(ext4_fs_t *fs, const char *oldp, const char *newp);

int ext4_link(ext4_fs_t *fs, const char *oldpath, const char *newpath,
		     int follow);

/* ---- hash.c ------------------------------------------------------------ */

uint32_t ext4_dx_hash(const ext4_fs_t *fs, int version, const char *name,
			     int len, uint32_t *minor_out);

/* ---- dir.c ------------------------------------------------------------- */

long ext4_readdir_impl(vfs_file_t *f, void *buf, long bytes);

/* ---- file.c ------------------------------------------------------------ */

int ext4_open_impl(ext4_fs_t *fs, const char *path, int flags, unsigned cmode,
			  vfs_file_t **out);

int ext4_stat_fill(ext4_fs_t *fs, unsigned long ino, struct kstat *st);

int ext4_stat_vfs_impl(ext4_fs_t *fs, const char *path, struct kstat *st);

long ext4_read_at_impl(vfs_file_t *f, void *buf, long bytes,
			      unsigned long pos, pc_readahead_t *ra);

long ext4_read_impl(vfs_file_t *f, void *buf, long bytes);

long ext4_seek_impl(vfs_file_t *f, long offset, int whence);

int ext4_close_impl(vfs_file_t *f);

int ext4_chdir_impl(ext4_fs_t *fs, const char *path);

int ext4_zero_range(ext4_fs_t *fs, unsigned long chain_id,
			   unsigned long file_size, unsigned long from,
			   unsigned long to);

long ext4_write_impl_at(vfs_file_t *f, const void *buf, long bytes,
			       unsigned long *posp, int append);

/* ---- symlink.c --------------------------------------------------------- */

int ext4_symlink(ext4_fs_t *fs, const char *target, const char *linkpath);

int ext4_readlink(ext4_fs_t *fs, const char *path, char *buf, unsigned long bufsiz);

/* ---- xattr.c ----------------------------------------------------------- */

int ext4_io_release_if_owner(ext4_fs_t *fs, uint64_t task_id);

int ext4_xattr_name_index(const char *full, const char **suffix,
				 unsigned *slen);

int ext4_xattr_get_ino(ext4_fs_t *fs, unsigned long ino, int index,
			      const char *name, unsigned nlen, void *buf,
			      unsigned long bufsz);

int ext4_xattr_list_ino(ext4_fs_t *fs, unsigned long ino, char *buf,
			       unsigned long bufsz);

int ext4_xattr_set_ino(ext4_fs_t *fs, unsigned long ino, int index,
			      const char *name, unsigned nlen,
			      const void *value, unsigned long size, int flags,
			      int remove);

/* ---- ops.c ------------------------------------------------------------- */

int ext4_sync_fs(ext4_fs_t *fs);

extern const vfs_ops_t ext4_vfs_ops;

/* ---- journal.c --------------------------------------------------------- */

void ext4_txn_begin(ext4_fs_t *fs);

void ext4_txn_capture(ext4_fs_t *fs, unsigned long pbn, const void *buf);

int ext4_txn_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf);

int ext4_ckpt_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf);

void ext4_drop_journal_copy(ext4_fs_t *fs, unsigned long pbn);

int ext4_journal_recover(ext4_fs_t *fs, int *did_work, int dry);

/* The journal inode at mount: its superblock's geometry, and every block it
 * maps inside the filesystem and clear of other metadata; those blocks then
 * join the system zone, owned by the journal.  ST_OK (also when there is no
 * journal this driver would use), ST_INVALID (said) or ST_NOMEM. */
int ext4_journal_check(ext4_fs_t *fs);

/* Record that this transaction wrote data into `ino`, so the commit stores
 * that data before the metadata describing it.  Defined with the journal. */
void ext4_ordered_add(ext4_fs_t *fs, unsigned long ino);

void ext4_journal_flush(ext4_fs_t *fs); /* defined with the journal */

void ext4_txn_flush(ext4_fs_t *fs); /* defined with the journal code */

/* Commit the batch and mark the journal empty (sync, fsync, unmount):
 * ST_OK, or ST_IO with the log left for a later flush or mount. */
int ext4_journal_clean(ext4_fs_t *fs);

/* Abort the journal after an error (errors=remount-ro): what is not yet
 * committed is never committed, nothing more is written to the log, and
 * what the log already holds stays there for the next mount to replay. */
void ext4_journal_abort(ext4_fs_t *fs);

static inline int ext4_journal_aborted(const ext4_fs_t *fs)
{
	return fs->st && __atomic_load_n(&fs->st->j_aborted, __ATOMIC_ACQUIRE);
}

void ext4_commit_attach(ext4_fs_t *fs);

void ext4_commit_detach(ext4_fs_t *fs);

#endif /* LIKEOS_EXT4_INT_H */
