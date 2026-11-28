/* LikeOS - FAT filesystem driver (FAT12, FAT16, FAT32, long file names):
 * declarations shared by the files of the driver (kernel/fs/fat32/); not
 * for use outside it.
 *
 * What the driver does and how it keeps a volume consistent: fat32.h.  The
 * conventions it shares with the exFAT, NTFS and ISO9660 drivers (block ids,
 * the page cache, the write chokepoint, lazy arming, locking): exfat.h.
 * The driver is organised in these files:
 *
 *    fat_int.h  configuration, in-memory types
 *    misc.c     byte helpers, the long-name checksum, error policy
 *               (errors=), corruption reports, timestamps
 *    lock.c     the lock engine (private recursive rwsem, lock wrappers)
 *    devio.c    device I/O: bounded reads, THE write chokepoint, discard,
 *               sync, the guard block device the page cache writes through
 *    cache.c    metadata cache: the FAT region (every copy written, one
 *               read); chain decoding into extents
 *    fatent.c   FAT entries (12, 16 and 32 bits); the allocator: free
 *               count, free runs, deferred frees and marks
 *    nls.c      code pages, Unicode case folding, UTF-8 / Latin-1 <->
 *               UTF-16
 *    namei.c    long names, short names (8.3) and their generation; name
 *               lookup (dcache), path resolution; unlink, rmdir, mkdir,
 *               rename
 *    inode.c    boot sector: BIOS parameter block, FAT type, geometry,
 *               DOS 1.x floppies, the backup boot sector, FSInfo, the state
 *               byte; streams: handle table, key table, extents, LRU,
 *               lifetime; page-cache superblock operations (block_at & co.);
 *               stat; statfs, sync, the operations table; mount options;
 *               mount, unmount, remount, probe, the filesystem type, the root
 *    dir.c      directory reading: iterator, entry sets (long and short
 *               entries); directory modification: building entry sets, the
 *               directory write queue, free-slot search, directory growth;
 *               readdir
 *    commit.c   the commit machinery: arming, ordered commit, clean marking
 *    file.c     cluster chains of streams: grow, shrink, release; the data
 *               read / write paths, zero fill on growth; open, close, read,
 *               write, seek, truncate; attribute changes
 *    xattr.c    extended attributes
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_FAT_INT_H
#define LIKEOS_FAT_INT_H

#include <kernel/mm/vmscan.h>
#include <kernel/fs/fat32/fat32.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/mount.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/dirent.h>
#include <kernel/uapi/bug.h>

/* ===========================================================================
 * 1. Configuration and in-memory types
 * =========================================================================== */

/* Hashed per-stream rwsems: a collision merely serialises two unrelated
 * streams. */
#define FAT_SLOCKS 64

/* Buckets of the stream key table. */
#define FAT_KHASH 1024

/* Unreferenced streams stay for a quick reopen -- with their cached pages,
 * which are keyed by the stream's handle and go with it -- until memory
 * runs short: the reclaim thread forgets the oldest through the
 * filesystem's shrinker (fat_shrink_scan).  This is only a ceiling against
 * unbounded growth, not the number they are trimmed to. */
#define FAT_STREAM_LRU_MAX 65536

/* Largest extent array one stream may decode to (16 bytes each).  Beyond it
 * the stream is refused with ST_NOMEM rather than taking unbounded memory. */
#define FAT_MAX_EXTENTS (1u << 20)

/* Metadata cache: units of the FAT region.  A soft cap: dirty units cannot
 * be dropped, so the cache may exceed it until the next commit writes them. */
#define FAT_MC_MAX 512

#define FAT_MC_HASH 256

/* FAT readahead: units read in one transfer on a miss, so that walking a
 * long chain or counting free clusters costs one device command per 64 KB of
 * FAT. */
#define FAT_MC_READAHEAD 16

/* Most device sectors one unit spans (its dirty mask is 32 bits). */
#define FAT_MC_MAX_SECTORS 32

/* Deferred frees (runs) gathered before the commit that applies them.  The
 * operation that would exceed it commits early (always consistent). */
#define FAT_PENDING_FREE_MAX 8192

/* Deferred FAT writes (truncation end-of-chain marks). */
#define FAT_DEFERRED_FAT_MAX 1024

/* Links from the old last cluster of a growing chain to its new clusters
 * held back until the new clusters' entries are stored.  The operation that
 * would exceed it commits first. */
#define FAT_LINKS_MAX 4096

/* Directory write queue: cached directory pages changed by the running
 * operation, written at the commit. */
#define FAT_DIRW_MAX 32

/* Largest single device transfer the driver issues. */
#define FAT_MAX_IO_BYTES (256UL << 10)

/* Page size as a shift (PAGE_SIZE is 4096). */
#ifndef PAGE_SHIFT

#define PAGE_SHIFT 12

#endif

/* Path walks: ancestors remembered (rename's loop check). */
#define FAT_DEPTH_MAX 128

/* Largest buffer the zeroing helper writes with. */
#define FAT_ZERO_CHUNK (64UL << 10)

/* Writes copy at most this many pages before balancing dirty memory. */
#define FAT_BALANCE_EVERY 64

/* Directory "." and ".." and directory slots: readdir positions 0 and 1 are
 * the dot entries, slot N of the directory is position N + 2. */
#define FAT_DIRPOS_FIRST 2

/* Candidates one scan of a directory checks while a short name is
 * generated (hash tails). */
#define FAT_SFN_BATCH 64

/* Reasons a mount is read-only although read-write was asked for. */
enum fat_ro_reason {
	FAT_RO_NONE = 0,
	FAT_RO_REQUESTED, /* "ro" */
	FAT_RO_DEVICE, /* write-protected device */
	FAT_RO_BACKUP_BOOT, /* main boot sector unusable, backup used */
	FAT_RO_VERSION, /* FAT32 version not 0.0 */
	FAT_RO_GEOMETRY, /* the volume reaches past the device */
	FAT_RO_ERRORS, /* errors=remount-ro fired */
	FAT_RO_FAT_MEDIA, /* FAT[0] holds no media byte at all */
	FAT_RO_NREASONS
};

/* errors= */
#define FAT_ERRORS_RO 0

#define FAT_ERRORS_CONTINUE 1

#define FAT_ERRORS_PANIC 2

/* shortname= : how short names are shown and when a name needs long-name
 * entries.  DISPLAY_* and CREATE_* bits, combined per mode. */
#define FAT_SN_DISPLAY_LOWER 0x01

#define FAT_SN_DISPLAY_WIN95 0x02

#define FAT_SN_DISPLAY_WINNT 0x04

#define FAT_SN_CREATE_WIN95 0x10

#define FAT_SN_CREATE_WINNT 0x20

#define FAT_SN_LOWER (FAT_SN_DISPLAY_LOWER | FAT_SN_CREATE_WIN95)

#define FAT_SN_WIN95 (FAT_SN_DISPLAY_WIN95 | FAT_SN_CREATE_WIN95)

#define FAT_SN_WINNT (FAT_SN_DISPLAY_WINNT | FAT_SN_CREATE_WINNT)

#define FAT_SN_MIXED (FAT_SN_DISPLAY_WINNT | FAT_SN_CREATE_WIN95)

/* iocharset= */
#define FAT_IOCS_UTF8 0

#define FAT_IOCS_LATIN1 1

#define FAT_IOCS_ASCII 2

/* Default permission masks: files 0644, directories 0755. */
#define FAT_DEFAULT_FMASK 0133u

#define FAT_DEFAULT_DMASK 0022u

#define FAT_DEFAULT_CODEPAGE 437

/* Mount options (section 21). */
struct fat_opts {
	uint32_t uid;
	uint32_t gid;
	uint32_t fmask;
	uint32_t dmask;
	uint32_t allow_utime; /* 0xFFFFFFFF: not given */
	int perm_opts; /* any of uid=/gid=/umask=/fmask=/dmask= given */
	int errors; /* FAT_ERRORS_* */
	int shortname; /* FAT_SN_* */
	int codepage; /* OEM code page of short names */
	int iocharset; /* FAT_IOCS_* */
	int uni_xlate; /* escape unrepresentable characters (":xxxx") */
	int nonumtail; /* short names: the plain form before "~1" */
	int discard;
	int flush;
	int quiet;
	int showexec;
	int rodir;
	int sys_immutable;
	int usefree;
	int dos1xfloppy;
	int tz_utc;
	int time_offset; /* minutes east of UTC */
	int time_offset_given;
	int fat_bits; /* fat=12|16|32, 0 = detect */
};

/* ---- The private recursive rwsem (ext4's semantics, own code) ----------- */

/* Tasks whose shared holds one rwsem records at a time. */
#define FAT_RWSEM_TRACK 32

typedef struct fat_rwsem {
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
	/* Shared holds, attributed to their tasks (FAT_RWSEM_TRACK tasks at a
	 * time; holds beyond that are only counted), so that
	 * release_locks_for_task() can take a dead task's shared holds back as
	 * well as its exclusive one. */
	int untracked;
	struct {
		uint64_t task;
		int count; /* 0: slot free */
	} rh[FAT_RWSEM_TRACK];
	spinlock_t lock; /* protects the fields above */
	const char *name;
} fat_rwsem_t;

/* One extent: `len' clusters of the stream starting at stream cluster index
 * `fidx' live at disk clusters `disk' .. `disk' + len - 1. */
typedef struct fat_extent {
	uint32_t fidx;
	uint32_t disk;
	uint32_t len;
	uint32_t _pad;
} fat_extent_t;

/* A stream: one file or directory. */
typedef struct fat_stream fat_stream_t;

struct fat_stream {
	struct fat_fs *fs;
	uint32_t handle; /* page-cache / icache chain handle */
	volatile int refs; /* under fs->st_lock */
	/* Location key (parent directory's first cluster, short entry slot);
	 * FAT_KEY_ROOT for the root directory; meaningless once orphaned. */
	uint64_t key;
	fat_stream_t *parent; /* held; NULL for the root and orphans */
	uint32_t entry_index; /* slot of the short entry in the parent */
	uint8_t lfn_count; /* long-name entries in front of it */

	/* What the short entry says (and what will be written back). */
	uint8_t name83[11];
	uint8_t attr;
	uint8_t ntres;
	uint8_t ctime_cs;
	uint16_t ctime, cdate, adate, mtime, mdate;
	uint32_t first_cluster;
	uint64_t size; /* files: the size field; directories: the chain */

	/* The allocation, decoded. */
	uint32_t nclusters;
	fat_extent_t *ext;
	uint32_t next;
	uint32_t ext_cap;

	unsigned is_dir : 1;
	unsigned is_root : 1;
	unsigned fixed_root : 1; /* the FAT12/16 root directory region */
	unsigned orphan : 1; /* unlinked; freed at the last close */
	unsigned meta_dirty : 1; /* short entry differs from memory */
	unsigned data_dirty : 1; /* pages dirtied since the last flush */
	unsigned on_dirty_list : 1;
	unsigned on_lru : 1;
	unsigned hashed : 1; /* in the key table */
	unsigned dead : 1; /* removed from the tables, being freed */
	unsigned chain_bad : 1; /* chain corrupt: nothing may be freed */

	int opens; /* open handles (under meta excl or st_lock) */

	/* Cached count of subdirectories (stat's st_nlink), -1 unknown. */
	long subdirs;

	/* Pages dirtied since the last commit stored them: [dlo, dhi]
	 * (dhi < dlo: none). */
	unsigned long dlo, dhi;
	/* A write-back error of this stream's data, for fsync / close. */
	int wb_err;

	fat_stream_t *hnext; /* key table chain */
	fat_stream_t *lru_prev, *lru_next;
	fat_stream_t *dnext; /* dirty list */
};

#define FAT_KEY_ROOT 0xFFFFFFFFFFFFFFFFULL

#define FAT_KEY(dir_cluster, idx) \
	((((uint64_t)(dir_cluster)) << 24) | ((uint64_t)(idx) & 0xFFFFFFULL))

/* The dcache parent key of the FAT12/16 root directory (it has no cluster;
 * cluster numbers start at 2). */
#define FAT_DKEY_FIXED_ROOT 1UL

/* Metadata cache unit (FAT region). */
typedef struct fat_mcu {
	uint32_t dirty; /* bit per device sector */
	uint16_t nsect; /* device sectors in this unit */
	uint16_t _pad;
	uint32_t unit; /* unit index within the FAT */
	uint32_t nbytes; /* valid bytes (nsect * dev_ss) */
	volatile int refs;
	uint64_t off; /* byte offset of the unit inside one FAT */
	uint8_t *data;
	struct fat_mcu *hnext;
	struct fat_mcu *lru_prev, *lru_next;
} fat_mcu_t;

/* A queued directory write: bytes [lo, hi) of a held cached page of a
 * directory stream, written at the next commit. */
struct fat_dirw {
	pc_page_t *pg;
	fat_stream_t *d; /* held */
	unsigned long pidx;
	unsigned lo, hi;
};

/* A deferred free: `len' clusters from `start'. */
struct fat_run {
	uint32_t start;
	uint32_t len;
};

/* A deferred FAT write. */
struct fat_fatw {
	uint32_t cluster;
	uint32_t value;
};

/* The code pages short names can be stored in. */
struct fat_codepage {
	int number;
	const uint16_t *high; /* Unicode of bytes 0x80..0xFF, 0 = none */
};

typedef struct fat_fs fat_fs_t;

struct fat_fs {
	/* The page cache's view.  sb.fs_private points back here. */
	vfs_superblock_t sb;
	/* The device the page cache sees: writes go through the chokepoint
	 * (section 4).  Found back from its address with offsetof. */
	block_device_t guard;
	block_device_t *bdev; /* the real device */
	vfs_mount_t *mnt;
	int is_root; /* the root filesystem: never unmounted */

	/* ---- geometry (fixed after mount) ---- */
	unsigned dev_ss; /* device sector bytes */
	unsigned dev_shift;
	unsigned fs_ss; /* logical sector bytes (BPB) */
	unsigned fs_shift;
	unsigned cs_shift; /* cluster bytes = 1 << cs_shift */
	unsigned long cs;
	unsigned bs_shift; /* exposed block bytes = 1 << bs_shift */
	unsigned long bs;
	unsigned bpc_shift; /* exposed blocks per cluster */
	unsigned long mcu_bytes; /* metadata cache unit size */
	int fat_bits; /* 12, 16 or 32 */
	uint64_t vol_dev_sectors; /* sectors of the volume on the device */
	uint64_t fat_lba[FAT_MAX_FATS]; /* each FAT copy, device sectors */
	unsigned nfats;
	unsigned active_fat; /* the copy read */
	int mirror; /* every copy is written */
	uint64_t fat_bytes; /* bytes of one FAT */
	uint64_t fat_used_bytes; /* bytes holding entries 0 .. cluster_end-1 */
	uint64_t root_lba; /* FAT12/16: the root directory region */
	uint32_t root_entries;
	uint64_t root_dev_sectors;
	uint64_t data_lba; /* cluster 2 */
	uint64_t data_dev_sectors;
	uint32_t cluster_count;
	uint32_t cluster_end; /* cluster_count + 2: one past the last */
	uint32_t root_cluster; /* FAT32 */
	uint32_t eoc_min, eoc_val, bad_val, ent_mask;
	uint64_t boot_dev_sectors; /* device sectors of the boot sector */
	uint64_t fsinfo_lba; /* 0: none */
	uint32_t serial;
	uint8_t media;
	uint8_t state_byte; /* the boot sector's state byte at mount */
	unsigned state_off; /* its offset */
	uint8_t label[11];
	uint16_t fsinfo_free_mount, _pad16;
	uint32_t fsinfo_free; /* FSInfo free count as found */
	uint32_t fsinfo_next;
	int fsinfo_valid;
	const struct fat_codepage *cp;

	/* ---- state ---- */
	int ro; /* read-only: requested or gated */
	unsigned gates; /* bit (1 << enum fat_ro_reason) per reason */
	int ro_latched; /* errors=remount-ro fired while mounted */
	volatile int armed; /* the dirty marks are on the device */
	/* A mutation passed the write checks: the chokepoint may arm the
	 * mount right before the first real write.  An operation that ends up
	 * changing nothing writes nothing. */
	int may_write;
	int in_commit; /* the commit runs: never nested */
	uint64_t io_errors; /* failed or refused writes (sticky count) */
	int was_dirty; /* the volume was marked dirty when mounted */
	int barrier_needed; /* direct writes since the last device sync */
	uint64_t errors_seen;
	unsigned refused_logged; /* refused-write log lines so far */
	int fsinfo_dirty; /* the free count / hint changed since stored */

	struct fat_opts opts;

	/* ---- locks ---- */
	fat_rwsem_t meta;
	fat_rwsem_t alloc;
	fat_rwsem_t slock[FAT_SLOCKS];
	spinlock_t st_lock; /* stream tables, refs, LRU */
	spinlock_t mc_lock; /* metadata cache */

	/* ---- streams ---- */
	fat_stream_t ***htab;
	uint32_t hfree_head;
	uint32_t next_handle; /* first never-used handle */
	fat_stream_t *khash[FAT_KHASH];
	fat_stream_t *lru_head, *lru_tail; /* head = most recent */
	unsigned lru_count;
	/* Gives unreferenced streams back under memory pressure. */
	struct vm_shrinker shrinker;
	unsigned nstreams;
	fat_stream_t *root;
	fat_stream_t *dirty_head; /* streams with meta_dirty (held) */
	/* Orphans whose last reference went while the metadata lock was not
	 * held exclusive: destroyed at the end of the next exclusive hold. */
	fat_stream_t *reap_head;

	/* ---- metadata cache ---- */
	fat_mcu_t *mc_hash[FAT_MC_HASH];
	fat_mcu_t *mc_lru_head, *mc_lru_tail;
	unsigned mc_count;
	unsigned mc_dirty;

	/* ---- allocator ---- */
	int64_t free_clusters; /* -1: not counted yet */
	uint32_t alloc_cursor; /* next-fit start (cluster number) */

	/* ---- deferred work of the running commit window ---- */
	struct fat_run *pf;
	unsigned npf, pf_cap;
	uint64_t pf_clusters;
	struct fat_fatw *dfat;
	unsigned ndfat, dfat_cap;
	/* links into new clusters, with the value each entry had before */
	struct fat_fatw *lnk;
	unsigned nlnk, lnk_cap;
	int mc_flushing; /* fat_mc_flush running (no early flush inside) */
	struct fat_dirw dirw[FAT_DIRW_MAX];
	unsigned ndirw;

	/* ---- commit scratch (metadata lock exclusive) ---- */
	uint8_t wb_set[FAT_SET_MAX_BYTES];

	/* ---- statistics (informational) ---- */
	uint64_t st_dev_writes;
	uint64_t st_dev_reads;
	uint64_t st_discards;
	uint64_t st_commits;
};

/* An open file or directory. */
typedef struct fat_file {
	vfs_file_t vfs; /* first: the VFS's handle is ours */
	fat_fs_t *fs;
	fat_stream_t *st; /* held */
	ic_inode_t *ic; /* the inode cache's entry for the stream */
	unsigned long pos;
	uint64_t dirpos; /* readdir: 0 ".", 1 "..", N + 2 slot N */
	uint64_t parent_ino; /* ".." of a directory */
	pc_readahead_t ra;
	int open_flags;
	int wrote; /* written through this handle since the last commit */
	int werr; /* deferred write error for fsync / close */
} fat_file_t;

/* What a directory lookup found: one entry set. */
typedef struct fat_dentry_info {
	uint32_t index; /* short entry slot */
	uint8_t lfn_count; /* valid long-name entries in front of it */
	uint8_t name83[11];
	uint8_t attr;
	uint8_t ntres;
	uint8_t ctime_cs;
	uint8_t chksum;
	uint16_t ctime, cdate, adate, mtime, mdate;
	uint16_t lname_len; /* units of the long name, 0: none */
	uint32_t first_cluster;
	uint32_t size;
} fat_dentry_info_t;

/* UTF-16 name buffers: room for every unit 20 long-name entries carry
 * (260 -- a set of 255 units fills the last entry with the terminator and
 * padding, and a hostile one fills all of it) plus a terminator. */
#define FAT_LNAME_UNITS (FAT_LFN_MAX_ENTRIES * FAT_LFN_CHARS + 2)

/* Per-operation scratch: buffers too big for the kernel stack. */
typedef struct fat_ctx {
	uint8_t set[FAT_SET_MAX_BYTES]; /* an entry set */
	uint8_t set2[FAT_SET_MAX_BYTES]; /* a second one (rename) */
	uint8_t set3[FAT_SET_MAX_BYTES]; /* scratch of a clear (rename) */
	uint16_t name[FAT_LNAME_UNITS]; /* a name, UTF-16 */
	uint16_t name2[FAT_LNAME_UNITS]; /* a stored long name */
	uint16_t up[FAT_LNAME_UNITS]; /* the name looked for, folded */
	uint16_t sname[16]; /* a short name as shown */
	uint16_t sup[16]; /* ... folded */
	char u8[FAT_NAME_MAX * 3 + 8]; /* a name, external form */
	char key[FAT_NAME_MAX * 3 + 8]; /* a folded dcache key */
	char key2[FAT_NAME_MAX * 3 + 8]; /* a second one (rename) */
	char skey[48]; /* the folded key of a short name */
	uint16_t tmpu[FAT_LNAME_UNITS]; /* scratch units (dcache keys) */
	char tmpk[FAT_NAME_MAX * 3 + 8]; /* scratch key */
	uint8_t sfn[11]; /* a short name being made */
	uint8_t sfn_cand[FAT_SFN_BATCH][11]; /* candidates of one scan */
	uint8_t sfn_taken[FAT_SFN_BATCH];
	uint32_t anc[FAT_DEPTH_MAX];
	unsigned nanc;
} fat_ctx_t;

static inline uint16_t fat_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t fat_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline void fat_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void fat_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static inline uint64_t fat_min64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

/* a + b without wrapping: 0 when it would. */
static inline int fat_add_ok(uint64_t a, uint64_t b, uint64_t *out)
{
	if (a + b < a)
		return 0;
	*out = a + b;
	return 1;
}

static inline uint64_t fat_round_up(uint64_t v, uint64_t align)
{
	return (v + align - 1) & ~(align - 1);
}

static inline int fat_is_pow2(unsigned long v)
{
	return v && !(v & (v - 1));
}

static inline uint64_t fat_task_id(void)
{
	task_t *cur = sched_current();

	return cur ? cur->id : 0;
}

void fat_rwsem_read_lock(fat_rwsem_t *s);

static inline void fat_meta_rlock(fat_fs_t *fs)
{
	fat_rwsem_read_lock(&fs->meta);
}

void fat_rwsem_read_unlock(fat_rwsem_t *s);

static inline void fat_meta_runlock(fat_fs_t *fs)
{
	fat_rwsem_read_unlock(&fs->meta);
}

void fat_rwsem_write_lock(fat_rwsem_t *s);

static inline void fat_meta_wlock(fat_fs_t *fs)
{
	fat_rwsem_write_lock(&fs->meta);
}

void fat_rwsem_write_unlock(fat_rwsem_t *s);

void fat_stream_maint(fat_fs_t *fs);

/* The end of an exclusive hold of a filesystem operation: parked orphans are
 * destroyed and the stream LRU trimmed while it is still held (the page
 * cache's writeback unlocks without this, see fat_sb_unlock_io). */
static inline void fat_meta_wunlock(fat_fs_t *fs)
{
	if (fs->meta.wdepth == 1 && fs->meta.owner == fat_task_id() &&
	    (fs->reap_head || fs->lru_count > FAT_STREAM_LRU_MAX))
		fat_stream_maint(fs);
	fat_rwsem_write_unlock(&fs->meta);
}

/* Mutator entry check: the metadata lock is held exclusive by this task.
 * lockdep_assert_held() sees that the lock is held at all; the owner check
 * catches a shared hold where an exclusive one is required. */
#define fat_assert_excl(fs)                                              \
	do {                                                             \
		lockdep_assert_held(&(fs)->meta);                        \
		WARN_ON_ONCE(!fat_rwsem_held_excl(&(fs)->meta));         \
	} while (0)

#define fat_assert_meta(fs)                                              \
	do {                                                             \
		lockdep_assert_held(&(fs)->meta);                        \
	} while (0)

int fat_rwsem_held_excl(fat_rwsem_t *s);

static inline void fat_alloc_lock(fat_fs_t *fs)
{
	fat_assert_excl(fs);
	fat_rwsem_write_lock(&fs->alloc);
}

static inline void fat_alloc_unlock(fat_fs_t *fs)
{
	fat_rwsem_write_unlock(&fs->alloc);
}

/* What a write may touch. */
#define FAT_W_BOOT 1 /* the boot sector (its state byte) */

#define FAT_W_FSINFO 2 /* the FSInfo sector */

#define FAT_W_FAT 3 /* the FAT copies */

#define FAT_W_ROOT 4 /* the fixed root directory region (FAT12/16) */

#define FAT_W_DATA 5 /* the data area (file and directory clusters) */

/* Corrupt on-disk metadata met: rate-limited warning, the errors= policy,
 * and the status the caller returns. */
#define fat_corrupt(fs, where, fmt, ...)                                  \
	({                                                                \
		WARN_RATELIMIT(1, "fat: corrupt volume: " fmt,            \
			       ##__VA_ARGS__);                            \
		fat_fs_error((fs), "corrupt metadata", (uint32_t)(where)); \
		ST_IO;                                                    \
	})

/* May the instance be modified now?  ST_OK or ST_ROFS. */
static inline int fat_writable(fat_fs_t *fs)
{
	if (fs->ro || fs->ro_latched)
		return ST_ROFS;
	return ST_OK;
}

/* Device sector of byte `off' of FAT copy `copy'. */
static inline uint64_t fat_mc_lba(fat_fs_t *fs, unsigned copy, uint64_t off)
{
	return fs->fat_lba[copy] + (off >> fs->dev_shift);
}

static inline int fat_cluster_ok(fat_fs_t *fs, uint64_t cl)
{
	return cl >= FAT_FIRST_CLUSTER && cl < fs->cluster_end;
}

/* A FAT reading cursor: keeps the last unit held across lookups, so a chain
 * walk touches the cache once per unit. */
struct fat_fatcur {
	fat_mcu_t *u;
};

#define FAT_LOWCASE_PAIRS 1136

extern const uint16_t fat_lowcase_tab[FAT_LOWCASE_PAIRS][2];

uint16_t fat_pair_lookup(const uint16_t (*tab)[2], unsigned n, uint16_t c);

/* The lower-case form of a code unit (itself when it has none). */
static inline uint16_t fat_lowc(uint16_t c)
{
	if (c < 0x80)
		return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c;
	return fat_pair_lookup(fat_lowcase_tab, FAT_LOWCASE_PAIRS, c);
}

/* The character of byte `b' in the code page, 0 when it has none. */
static inline uint16_t fat_cp_to_uni(const struct fat_codepage *cp, uint8_t b)
{
	if (b < 0x80)
		return b;
	return cp->high[b - 0x80];
}

static inline unsigned long fat_chain_of(const fat_stream_t *st)
{
	return FAT_BID_ENC(st->handle, 0);
}

/* The first cluster a directory stream gives its children's keys (0 for
 * the FAT12/16 root). */
static inline uint32_t fat_dir_keycl(const fat_stream_t *d)
{
	return d->fixed_root ? 0 : d->first_cluster;
}

/* The dcache parent key of a directory stream: its first cluster (stable
 * for its life), FAT_DKEY_FIXED_ROOT for the FAT12/16 root. */
static inline unsigned long fat_dkey(const fat_stream_t *d)
{
	return d->fixed_root ? FAT_DKEY_FIXED_ROOT :
			       (unsigned long)d->first_cluster;
}

/* The last disk cluster of the stream, 0 when it has none. */
static inline uint32_t fat_st_last(const fat_stream_t *st)
{
	if (!st->next)
		return 0;
	return st->ext[st->next - 1].disk + st->ext[st->next - 1].len - 1;
}

/* First device sector of cluster `cl'. */
static inline uint64_t fat_cl_lba(fat_fs_t *fs, uint32_t cl)
{
	return fs->data_lba + ((uint64_t)(cl - FAT_FIRST_CLUSTER)
			       << (fs->cs_shift - fs->dev_shift));
}

typedef struct fat_diter {
	fat_fs_t *fs;
	fat_stream_t *d;
	pc_page_t *pg;
	unsigned long pidx;
	int err;
} fat_diter_t;

/* Number of entry slots of a directory. */
static inline uint64_t fat_dir_slots(const fat_stream_t *d)
{
	return d->size >> FAT_DENTRY_SHIFT;
}

static inline int fat_de_is_lfn(const uint8_t *e)
{
	return (e[FAT_DE_ATTR] & FAT_ATTR_LFN_MASK) == FAT_ATTR_LFN;
}

/* An entry in use (neither the end marker nor free). */
static inline int fat_de_in_use(const uint8_t *e)
{
	return e[0] != FAT_NAME_END && e[0] != FAT_NAME_DELETED;
}

int fat_sfn_is_dot(const uint8_t *nm);

/* A short entry in use that names a file or directory: not a long-name
 * entry, not the volume label, not "." or "..". */
static inline int fat_de_is_object(const uint8_t *e)
{
	return fat_de_in_use(e) && !fat_de_is_lfn(e) &&
	       !(e[FAT_DE_ATTR] & FAT_ATTR_VOLUME) && !fat_sfn_is_dot(e);
}

/* The first cluster a short entry names. */
static inline uint32_t fat_de_cluster(fat_fs_t *fs, const uint8_t *e)
{
	uint32_t c = fat_le16(e + FAT_DE_CLUS_LO);

	if (fs->fat_bits == 32)
		c |= (uint32_t)fat_le16(e + FAT_DE_CLUS_HI) << 16;
	return c;
}

static inline void fat_de_set_cluster(fat_fs_t *fs, uint8_t *e, uint32_t c)
{
	fat_put16(e + FAT_DE_CLUS_LO, (uint16_t)c);
	if (fs->fat_bits == 32)
		fat_put16(e + FAT_DE_CLUS_HI, (uint16_t)(c >> 16));
	else
		fat_put16(e + FAT_DE_CLUS_HI, 0);
}

unsigned long fat_strlen(const char *s);

static inline int fat_dcache_usable(const char *key)
{
	return key[0] && fat_strlen(key) <= DC_NAME_MAX;
}

/* What a path resolves to. */
struct fat_walk {
	fat_stream_t *dir; /* held: directory of the last component (NULL
			    * when the path is the root) */
	fat_stream_t *st; /* held: the object, when fat_walk_obj() got it */
	fat_dentry_info_t di; /* the last component's entry */
	int found; /* the last component exists */
	int is_root;
	int trailing_slash; /* "name/" */
	const char *last; /* the last component in the path */
	unsigned long last_len;
	int nlen; /* the last component's length in UTF-16 units */
};

/* Note page `pidx' of `st' as dirtied (the range the commit checks). */
static inline void fat_note_dirty(fat_stream_t *st, unsigned long pidx)
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

/* ---- misc.c ------------------------------------------------------------ */

unsigned fat_ilog2(unsigned long v);

int fat_streq(const char *a, const char *b);

int fat_streq_ci(const char *a, const char *b);

uint8_t fat_lfn_checksum(const uint8_t name[11]);

const char *fat_name_of(fat_fs_t *fs);

/* Forward declarations of the cross-section helpers. */
void fat_fs_error(fat_fs_t *fs, const char *what, uint32_t where);

void fat_time_encode(fat_fs_t *fs, int64_t sec, long nsec,
			    uint16_t *time, uint16_t *date, uint8_t *cs10);

void fat_time_decode(fat_fs_t *fs, uint16_t time, uint16_t date,
			    uint8_t cs10, uint64_t *sec, uint64_t *nsec);

void fat_now(fat_fs_t *fs, uint16_t *time, uint16_t *date, uint8_t *cs10);

/* ---- lock.c ------------------------------------------------------------ */

void fat_rwsem_init(fat_rwsem_t *s, const char *name);

int fat_rwsem_release_owner(fat_rwsem_t *s, uint64_t task_id);

void fat_slock_shared(fat_fs_t *fs, uint32_t h);

void fat_sunlock_shared(fat_fs_t *fs, uint32_t h);

void fat_slock_excl(fat_fs_t *fs, uint32_t h);

void fat_sunlock_excl(fat_fs_t *fs, uint32_t h);

/* ---- devio.c ----------------------------------------------------------- */

int fat_dev_read(fat_fs_t *fs, uint64_t lba, uint64_t count, void *buf);

int fat_dev_write(fat_fs_t *fs, uint64_t lba, uint64_t count,
			 const void *buf, int kind);

int fat_dev_discard(fat_fs_t *fs, uint64_t lba, uint64_t count);

int fat_dev_sync(fat_fs_t *fs);

void fat_guard_init(fat_fs_t *fs);

int fat_dev_zero(fat_fs_t *fs, uint64_t lba, uint64_t count);

/* ---- cache.c ----------------------------------------------------------- */

fat_mcu_t *fat_mc_get(fat_fs_t *fs, uint32_t unit, int *err);

void fat_mc_put(fat_fs_t *fs, fat_mcu_t *u);

void fat_mc_dirty(fat_fs_t *fs, fat_mcu_t *u, unsigned off, unsigned len);

int fat_mc_flush_region(fat_fs_t *fs);

void fat_mc_destroy(fat_fs_t *fs);

int fat_ext_append(fat_extent_t **ext, uint32_t *next, uint32_t *cap,
			  uint32_t fidx, uint32_t disk, uint32_t len);

int fat_chain_decode(fat_fs_t *fs, uint32_t first, uint64_t want,
			    uint64_t max, int quiet, fat_extent_t **ext,
			    uint32_t *next, uint32_t *cap, uint32_t *ncl);

uint32_t fat_ext_lookup(const fat_extent_t *ext, uint32_t n, uint64_t ci);

/* ---- fatent.c ---------------------------------------------------------- */

uint64_t fat_ent_bytes(int bits, uint64_t n);

void fat_fatcur_done(fat_fs_t *fs, struct fat_fatcur *c);

int fat_ent_locate(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
			  unsigned *in);

int fat_ent_get(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
		       uint32_t *val);

int fat_ent_put(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
		       uint32_t val);

int fat_links_swap(fat_fs_t *fs, unsigned *done);

int fat_link_reserve(fat_fs_t *fs);

int fat_link_note(fat_fs_t *fs, uint32_t cl, uint32_t old);

void fat_link_forget(fat_fs_t *fs, uint32_t cl);

int fat_mc_flush(fat_fs_t *fs);

int fat_fat_read(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
			uint32_t *val);

int fat_fat_write(fat_fs_t *fs, struct fat_fatcur *c, uint32_t cl,
			 uint32_t val);

int fat_fat_defer(fat_fs_t *fs, uint32_t cl, uint32_t val);

int fat_fat_deferred(fat_fs_t *fs, uint32_t cl, uint32_t *val);

int fat_count_free(fat_fs_t *fs);

void fat_free_count_add(fat_fs_t *fs, int64_t delta);

void fat_unmark_run(fat_fs_t *fs, uint32_t start, uint32_t len);

int fat_alloc(fat_fs_t *fs, uint32_t last, uint32_t n,
		     struct fat_run **runs, unsigned *nruns);

int fat_free_extents(fat_fs_t *fs, const fat_extent_t *ext, uint32_t n);

int fat_apply_frees(fat_fs_t *fs);

/* ---- nls.c ------------------------------------------------------------- */

void fat_fold(const uint16_t *in, unsigned n, uint16_t *out);

int fat_name_eq(const uint16_t *a, unsigned na, const uint16_t *up,
		       unsigned n);

const struct fat_codepage *fat_cp_find(int number);

uint8_t fat_cp_from_uni(const struct fat_codepage *cp, uint16_t u);

uint8_t fat_cp_toupper(const struct fat_codepage *cp, uint8_t b);

int fat_utf16_to_utf8(const uint16_t *in, unsigned n, char *out,
			     unsigned cap);

int fat_ext_to_utf16(fat_fs_t *fs, const char *s, unsigned long len,
			    uint16_t *out, unsigned max);

int fat_utf16_to_ext(fat_fs_t *fs, const uint16_t *in, unsigned n,
			    char *out, unsigned cap);

int fat_lfn_bad_char(uint16_t c);

/* ---- namei.c ----------------------------------------------------------- */

int fat_name_prepare(fat_fs_t *fs, fat_ctx_t *ctx, const char *comp,
			    unsigned long len, int for_create);

unsigned fat_sfn_display(fat_fs_t *fs, const uint8_t *nm, uint8_t ntres,
				uint16_t *out);

int fat_sfn_make(fat_fs_t *fs, fat_ctx_t *ctx, fat_stream_t *d,
			const uint16_t *name, unsigned n, uint8_t *lcase,
			int64_t exclude);

void fat_dc_forget(fat_fs_t *fs, fat_stream_t *d, const char *key);

void fat_dc_remember(fat_fs_t *fs, fat_stream_t *d, const char *key,
			    const fat_dentry_info_t *di);

void fat_dc_forget_set(fat_fs_t *fs, fat_stream_t *d,
			      const fat_dentry_info_t *di,
			      const uint16_t *lname, char *kbuf, uint16_t *ubuf);

void fat_walk_release(struct fat_walk *w);

int fat_walk(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
		    struct fat_walk *w);

int fat_walk_obj(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
			struct fat_walk *w);

fat_ctx_t *fat_ctx_new(void);

void fat_ctx_free(fat_ctx_t *ctx);

int fat_dirw_add(fat_fs_t *fs, fat_stream_t *d, unsigned long pidx,
			pc_page_t **pg, unsigned lo, unsigned hi);

int fat_unlink_op(vfs_mount_t *m, const char *path);

int fat_rmdir_op(vfs_mount_t *m, const char *path);

int fat_mkdir_op(vfs_mount_t *m, const char *path, unsigned int mode);

int fat_rename_op(vfs_mount_t *m, const char *oldpath,
			 const char *newpath);

int fat_chdir_op(vfs_mount_t *m, const char *path);

/* ---- inode.c ----------------------------------------------------------- */

int fat_fsinfo_write(fat_fs_t *fs);

int fat_boot_write_state(fat_fs_t *fs, int dirty);

void fat_stream_hold(fat_stream_t *st);

void fat_stream_put(fat_stream_t *st);

fat_stream_t *fat_stream_get_ex(fat_fs_t *fs, fat_stream_t *parent,
				       const fat_dentry_info_t *di, int quiet,
				       int *err);

fat_stream_t *fat_stream_get(fat_fs_t *fs, fat_stream_t *parent,
				    const fat_dentry_info_t *di, int *err);

fat_stream_t *fat_stream_find(fat_fs_t *fs, uint64_t key);

void fat_stream_rekey(fat_fs_t *fs, fat_stream_t *st,
			     fat_stream_t *new_parent, uint32_t index,
			     uint8_t lfn_count);

void fat_stream_orphan(fat_fs_t *fs, fat_stream_t *st);

unsigned long fat_st_block_lba(fat_fs_t *fs, const fat_stream_t *st,
				      unsigned long n);

fat_fs_t *fat_fs_of(vfs_mount_t *m);

uint64_t fat_ino(uint32_t first, uint32_t dir_keycl, uint32_t index);

uint64_t fat_st_ino(fat_fs_t *fs, const fat_stream_t *st);

unsigned fat_mode_of(fat_fs_t *fs, uint8_t attr, const uint8_t *nm);

int fat_stat_locked(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
			   struct kstat *k);

extern const vfs_ops_t fat_vfs_ops;

/* ---- dir.c ------------------------------------------------------------- */

void fat_diter_init(fat_diter_t *it, fat_fs_t *fs, fat_stream_t *d);

void fat_diter_done(fat_diter_t *it);

const uint8_t *fat_diter_get(fat_diter_t *it, uint64_t idx);

int fat_diter_copy(fat_diter_t *it, uint64_t idx, unsigned count,
			  uint8_t *buf);

void fat_di_from_sfn(fat_fs_t *fs, const uint8_t *e, uint32_t idx,
			    fat_dentry_info_t *di);

int fat_set_read(fat_diter_t *it, uint64_t idx, fat_dentry_info_t *di,
			uint16_t *lname);

unsigned fat_set_name(fat_fs_t *fs, const fat_dentry_info_t *di,
			     const uint16_t *lname, uint16_t *out);

int fat_set_matches(fat_fs_t *fs, fat_ctx_t *ctx,
			   const fat_dentry_info_t *di, const uint16_t *lname,
			   unsigned n);

/* Mark which of ctx->sfn_cand[0 .. ncand) are short names already present
 * in directory `d' (ctx->sfn_taken).  The entry at slot `exclude' (the file
 * being renamed, whose name goes away) does not count.  Metadata lock held. */
int fat_sfn_scan(fat_fs_t *fs, fat_ctx_t *ctx, fat_stream_t *d,
			unsigned ncand, int64_t exclude);

int fat_dir_write(fat_fs_t *fs, fat_stream_t *d, uint64_t idx,
			 unsigned count, const uint8_t *buf);

int fat_dir_clear(fat_fs_t *fs, fat_stream_t *d, uint64_t idx,
			 unsigned count, uint8_t *scratch);

int fat_dirw_flush(fat_fs_t *fs);

unsigned fat_set_build(uint8_t *set, const uint16_t *name, unsigned len,
			      int need_lfn, const uint8_t *sfn, uint8_t lcase,
			      const uint8_t *proto);

void fat_proto_new(fat_fs_t *fs, uint8_t *e, uint8_t attr,
			  uint32_t first, uint32_t size);

int fat_slot_fits(fat_fs_t *fs, fat_stream_t *d, uint64_t idx,
			 unsigned count);

int fat_dir_alloc_slots(fat_fs_t *fs, fat_stream_t *d, unsigned need,
			       uint64_t *idx);

int fat_dir_empty(fat_fs_t *fs, fat_stream_t *d);

int fat_stream_store(fat_fs_t *fs, fat_stream_t *st);

void fat_stream_mark_dirty(fat_fs_t *fs, fat_stream_t *st);

long fat_readdir_op(vfs_file_t *f, void *ubuf, long ubytes);

/* ---- commit.c ---------------------------------------------------------- */

int fat_may_write(fat_fs_t *fs);

int fat_arm_now(fat_fs_t *fs);

void fat_commit_abandon(fat_fs_t *fs);

int fat_commit_queue(fat_fs_t *fs);

int fat_commit(fat_fs_t *fs);

int fat_clean(fat_fs_t *fs);

int fat_stream_append(fat_fs_t *fs, fat_stream_t *st,
			     const struct fat_run *runs, unsigned nruns,
			     int *pinned);

/* ---- file.c ------------------------------------------------------------ */

void fat_runs_unalloc(fat_fs_t *fs, const struct fat_run *runs,
			     unsigned nruns, uint32_t total, int pinned);

void fat_touch_mtime(fat_fs_t *fs, fat_stream_t *st);

int fat_place_set(fat_fs_t *fs, fat_ctx_t *ctx, struct fat_walk *w,
			 unsigned n, const uint8_t *proto,
			 fat_dentry_info_t *di);

int fat_fence(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
		     fat_stream_t **stp, int quiet);

void fat_unfence(fat_fs_t *fs, fat_stream_t *st);

int fat_open_op(vfs_mount_t *m, const char *path, int flags,
		       vfs_file_t **out);

int fat_open_mode_op(vfs_mount_t *m, const char *path, int flags,
			    unsigned int mode, vfs_file_t **out);

int fat_close_op(vfs_file_t *f);

long fat_read_op(vfs_file_t *f, void *buf, long bytes);

long fat_read_at_op(vfs_file_t *f, void *buf, long bytes, long off);

long fat_write_op(vfs_file_t *f, const void *buf, long bytes);

long fat_write_at_op(vfs_file_t *f, const void *buf, long bytes,
			    long off);

long fat_seek_op(vfs_file_t *f, long offset, int whence);

int fat_truncate_op(vfs_file_t *f, unsigned long size);

int fat_set_attr(fat_fs_t *fs, fat_stream_t *st, uint8_t attr);

int fat_chmod_op(vfs_mount_t *m, const char *path, unsigned int mode);

int fat_fchmod_op(vfs_file_t *f, unsigned int mode);

int fat_chown_op(vfs_mount_t *m, const char *path, int uid, int gid,
			int nofollow);

int fat_fchown_op(vfs_file_t *f, int uid, int gid);

int fat_utimens_op(vfs_mount_t *m, const char *path, int nofollow,
			  const struct vfs_utimes *t);

int fat_futimens_op(vfs_file_t *f, const struct vfs_utimes *t);

/* ---- xattr.c ----------------------------------------------------------- */

int fat_getxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			   const char *name, void *val, unsigned long size);

int fat_setxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			   const char *name, const void *val, unsigned long size,
			   int flags);

int fat_listxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			    char *list, unsigned long size);

int fat_removexattr_op(vfs_mount_t *m, const char *path, int nofollow,
			      const char *name);

int fat_fgetxattr_op(vfs_file_t *f, const char *name, void *val,
			    unsigned long size);

int fat_fsetxattr_op(vfs_file_t *f, const char *name, const void *val,
			    unsigned long size, int flags);

int fat_flistxattr_op(vfs_file_t *f, char *list, unsigned long size);

int fat_fremovexattr_op(vfs_file_t *f, const char *name);

#endif /* LIKEOS_FAT_INT_H */
