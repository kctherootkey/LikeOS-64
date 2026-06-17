// LikeOS-64 - ext4 filesystem driver
//
// Phase 1: read-only mount + extent/indirect block reads + directory
// traversal.  Plugs into the same generic cache layer (pagecache / dcache /
// icache) as FAT32 by implementing the two vtables vfs_ops_t (path/handle
// ops) and vfs_sb_ops_t (block ops the caches dispatch through).
//
// Block-id encoding (the crux of the cache integration)
// -----------------------------------------------------
// The generic caches seed a file's block chain with chain[0] = "start_cluster"
// and extend it via sb->ops->next_block(); they then map each chain element to
// a disk LBA via sb->ops->block_to_lba().  FAT32 can use the raw cluster number
// because cluster == inode == first-data-block.  ext4 cannot: an inode number
// is not a block number and the two numeric spaces overlap.
//
// We therefore make the chain element SELF-DESCRIBING: it encodes the pair
// (inode number, logical block index).  block_to_lba() and next_block() then
// become pure stateless functions of that encoding — no per-FS reverse map,
// no shared mutable state, trivially SMP-safe.
//
//   bit 63        : tag (always 1 for an ext4 chain id)
//   bits 62..32   : inode number (31 bits)
//   bits 31..0    : logical block index within the file (32 bits)
//
#ifndef LIKEOS_EXT4_H
#define LIKEOS_EXT4_H

#include <kernel/uapi/status.h>
#include <kernel/uapi/types.h>
#include <kernel/dev/block/block.h>
#include <kernel/fs/vfs.h>
#include <kernel/fs/vfs_sb.h>

/* ===================================================================
 * On-disk structures (little-endian; x86-64 is LE so direct access OK)
 * =================================================================== */

#define EXT4_SUPER_MAGIC          0xEF53
#define EXT4_SUPERBLOCK_OFFSET    1024      /* bytes from partition start   */
#define EXT4_ROOT_INO             2
#define EXT4_GOOD_OLD_INODE_SIZE  128
#define EXT4_GOOD_OLD_FIRST_INO   11
#define EXT4_NDIR_BLOCKS          12        /* direct block pointers        */
#define EXT4_IND_BLOCK            12
#define EXT4_DIND_BLOCK           13
#define EXT4_TIND_BLOCK           14
#define EXT4_N_BLOCKS             15

/* s_feature_incompat bits */
#define EXT4_FEATURE_INCOMPAT_COMPRESSION 0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE    0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER     0x0004  /* needs journal replay   */
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG     0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS     0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT       0x0080
#define EXT4_FEATURE_INCOMPAT_MMP         0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG     0x0200
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x2000

/* s_feature_compat bits */
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL  0x0004

/* s_feature_ro_compat bits */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE   0x0002
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE    0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM     0x0010
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400

/* inode i_flags bits */
#define EXT4_INODE_EXTENTS_FL     0x00080000
#define EXT4_INODE_INLINE_DATA_FL 0x10000000

/* extent header magic */
#define EXT4_EXT_MAGIC            0xF30A

/* dir_entry file_type values */
#define EXT4_FT_UNKNOWN  0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2
#define EXT4_FT_CHRDEV   3
#define EXT4_FT_BLKDEV   4
#define EXT4_FT_FIFO     5
#define EXT4_FT_SOCK     6
#define EXT4_FT_SYMLINK  7

typedef struct ext4_super_block {
    uint32_t s_inodes_count;            /* 0x000 */
    uint32_t s_blocks_count_lo;         /* 0x004 */
    uint32_t s_r_blocks_count_lo;       /* 0x008 */
    uint32_t s_free_blocks_count_lo;    /* 0x00C */
    uint32_t s_free_inodes_count;       /* 0x010 */
    uint32_t s_first_data_block;        /* 0x014 */
    uint32_t s_log_block_size;          /* 0x018 */
    uint32_t s_log_cluster_size;        /* 0x01C */
    uint32_t s_blocks_per_group;        /* 0x020 */
    uint32_t s_clusters_per_group;      /* 0x024 */
    uint32_t s_inodes_per_group;        /* 0x028 */
    uint32_t s_mtime;                   /* 0x02C */
    uint32_t s_wtime;                   /* 0x030 */
    uint16_t s_mnt_count;               /* 0x034 */
    uint16_t s_max_mnt_count;           /* 0x036 */
    uint16_t s_magic;                   /* 0x038 */
    uint16_t s_state;                   /* 0x03A */
    uint16_t s_errors;                  /* 0x03C */
    uint16_t s_minor_rev_level;         /* 0x03E */
    uint32_t s_lastcheck;               /* 0x040 */
    uint32_t s_checkinterval;           /* 0x044 */
    uint32_t s_creator_os;              /* 0x048 */
    uint32_t s_rev_level;               /* 0x04C */
    uint16_t s_def_resuid;              /* 0x050 */
    uint16_t s_def_resgid;              /* 0x052 */
    uint32_t s_first_ino;               /* 0x054 */
    uint16_t s_inode_size;              /* 0x058 */
    uint16_t s_block_group_nr;          /* 0x05A */
    uint32_t s_feature_compat;          /* 0x05C */
    uint32_t s_feature_incompat;        /* 0x060 */
    uint32_t s_feature_ro_compat;       /* 0x064 */
    uint8_t  s_uuid[16];                /* 0x068 */
    char     s_volume_name[16];         /* 0x078 */
    char     s_last_mounted[64];        /* 0x088 */
    uint32_t s_algorithm_usage_bitmap;  /* 0x0C8 */
    uint8_t  s_prealloc_blocks;         /* 0x0CC */
    uint8_t  s_prealloc_dir_blocks;     /* 0x0CD */
    uint16_t s_reserved_gdt_blocks;     /* 0x0CE */
    uint8_t  s_journal_uuid[16];        /* 0x0D0 */
    uint32_t s_journal_inum;            /* 0x0E0 */
    uint32_t s_journal_dev;             /* 0x0E4 */
    uint32_t s_last_orphan;             /* 0x0E8 */
    uint32_t s_hash_seed[4];            /* 0x0EC */
    uint8_t  s_def_hash_version;        /* 0x0FC */
    uint8_t  s_jnl_backup_type;         /* 0x0FD */
    uint16_t s_desc_size;               /* 0x0FE */
    uint32_t s_default_mount_opts;      /* 0x100 */
    uint32_t s_first_meta_bg;           /* 0x104 */
    uint32_t s_mkfs_time;               /* 0x108 */
    uint32_t s_jnl_blocks[17];          /* 0x10C */
    uint32_t s_blocks_count_hi;         /* 0x150 */
    uint32_t s_r_blocks_count_hi;       /* 0x154 */
    uint32_t s_free_blocks_count_hi;    /* 0x158 */
    uint16_t s_min_extra_isize;         /* 0x15C */
    uint16_t s_want_extra_isize;        /* 0x15E */
    uint32_t s_flags;                   /* 0x160 */
    uint8_t  s_padding[1024 - 0x164];   /* fill to 1024 bytes              */
} __attribute__((packed)) ext4_super_block;

typedef struct ext4_group_desc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    /* 64-bit fields (present only when s_desc_size >= 64) */
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed)) ext4_group_desc;

typedef struct ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint8_t  i_block[60];            /* extent tree root OR 15 block ptrs   */
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;            /* for regular files (high 32 of size) */
    uint32_t i_obso_faddr;
    uint8_t  i_osd2[12];
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed)) ext4_inode;      /* 160 bytes (covers 256-byte inode prefix) */

typedef struct ext4_extent_header {
    uint16_t eh_magic;              /* 0xF30A */
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;              /* 0 = leaf (extents); >0 = index nodes  */
    uint32_t eh_generation;
} __attribute__((packed)) ext4_extent_header;

typedef struct ext4_extent_idx {
    uint32_t ei_block;              /* covers logical blocks >= ei_block     */
    uint32_t ei_leaf_lo;            /* child block (low 32)                  */
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed)) ext4_extent_idx;

typedef struct ext4_extent {
    uint32_t ee_block;              /* first logical block this extent maps  */
    uint16_t ee_len;               /* number of blocks (>32768 => uninit)   */
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;          /* first physical block                  */
} __attribute__((packed)) ext4_extent;

typedef struct ext4_dir_entry_2 {
    uint32_t inode;                 /* 0 = unused slot                       */
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];                /* not NUL-terminated on disk            */
} __attribute__((packed)) ext4_dir_entry_2;

/* ===================================================================
 * Journal (jbd2) on-disk format.
 *
 * IMPORTANT: unlike the ext4 filesystem structures above (which are
 * little-endian and read directly on x86-64), the jbd2 journal is stored
 * **big-endian** on disk.  Every multi-byte field below must be byte-swapped
 * before use (see the be16/be32 helpers in ext4.c).
 * =================================================================== */

#define JBD2_MAGIC_NUMBER          0xc03b3998U

/* journal_header_t h_blocktype values */
#define JBD2_DESCRIPTOR_BLOCK      1
#define JBD2_COMMIT_BLOCK          2
#define JBD2_SUPERBLOCK_V1         3
#define JBD2_SUPERBLOCK_V2         4
#define JBD2_REVOKE_BLOCK          5

/* journal s_feature_incompat bits */
#define JBD2_FEATURE_INCOMPAT_REVOKE        0x00000001
#define JBD2_FEATURE_INCOMPAT_64BIT         0x00000002
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT  0x00000004
#define JBD2_FEATURE_INCOMPAT_CSUM_V2       0x00000008
#define JBD2_FEATURE_INCOMPAT_CSUM_V3       0x00000010
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT   0x00000020

/* journal_block_tag t_flags bits */
#define JBD2_FLAG_ESCAPE     1   /* on-disk block escaped (had jbd2 magic)   */
#define JBD2_FLAG_SAME_UUID  2   /* no 16-byte UUID follows this tag         */
#define JBD2_FLAG_DELETED    4
#define JBD2_FLAG_LAST_TAG   8   /* last tag in this descriptor block        */

/* Common header at the start of every journal log block (BE). */
typedef struct journal_header_s {
    uint32_t h_magic;            /* JBD2_MAGIC_NUMBER                         */
    uint32_t h_blocktype;        /* JBD2_*_BLOCK                              */
    uint32_t h_sequence;         /* transaction ID this block belongs to      */
} __attribute__((packed)) journal_header_t;

/* Journal superblock — lives in journal log block 0 (BE).  Only the early
 * fields are needed for replay; tail (uuid users / checksum) is ignored. */
typedef struct journal_superblock_s {
    journal_header_t s_header;          /* 0x00 (blocktype = SUPERBLOCK_V1/2) */
    uint32_t s_blocksize;               /* 0x0C journal block size (== fs bs) */
    uint32_t s_maxlen;                  /* 0x10 total blocks in the journal   */
    uint32_t s_first;                   /* 0x14 first block of log info (=1)   */
    uint32_t s_sequence;                /* 0x18 first commit ID expected      */
    uint32_t s_start;                   /* 0x1C log start block (0 => clean)   */
    uint32_t s_errno;                   /* 0x20                               */
    uint32_t s_feature_compat;          /* 0x24                               */
    uint32_t s_feature_incompat;        /* 0x28 JBD2_FEATURE_INCOMPAT_*       */
    uint32_t s_feature_ro_compat;       /* 0x2C                               */
    uint8_t  s_uuid[16];                /* 0x30                               */
} __attribute__((packed)) journal_superblock_t;

_Static_assert(__builtin_offsetof(journal_superblock_t, s_maxlen)  == 0x10, "jsb s_maxlen");
_Static_assert(__builtin_offsetof(journal_superblock_t, s_sequence)== 0x18, "jsb s_sequence");
_Static_assert(__builtin_offsetof(journal_superblock_t, s_start)   == 0x1C, "jsb s_start");
_Static_assert(__builtin_offsetof(journal_superblock_t, s_feature_incompat) == 0x28, "jsb incompat");

/* Revoke block: journal_header_t followed by r_count (BE, total used bytes in
 * the block including the header) and then an array of 4- or 8-byte block
 * numbers (8 when JBD2_FEATURE_INCOMPAT_64BIT). */
typedef struct jbd2_revoke_header_s {
    journal_header_t r_header;          /* blocktype = REVOKE_BLOCK           */
    uint32_t r_count;                   /* used bytes incl. this header       */
} __attribute__((packed)) jbd2_revoke_header_t;

/* ===================================================================
 * Block-id encoding for the generic cache chain (see file header)
 * =================================================================== */
#define EXT4_BID_TAG        (1UL << 63)
#define EXT4_BID_ENC(ino, lidx) \
    (EXT4_BID_TAG | (((unsigned long)(ino) & 0x7FFFFFFFUL) << 32) \
                  | ((unsigned long)(lidx) & 0xFFFFFFFFUL))
#define EXT4_BID_INO(b)     (((b) >> 32) & 0x7FFFFFFFUL)
#define EXT4_BID_LIDX(b)    ((b) & 0xFFFFFFFFUL)
#define EXT4_BID_IS(b)      (((b) & EXT4_BID_TAG) != 0)
/* end-of-chain: any value >= this marker (or 0) means "no more blocks".
 * Real encoded ids have bit 63 set but bit 62 clear (ino is 31 bits but we
 * never approach 2^31 inodes), so they all sit below this marker. */
#define EXT4_EOC_MARKER     0xC000000000000000UL
#define EXT4_BID_EOC        0xFFFFFFFFFFFFFFFFUL
/* physical-block sentinel for a sparse hole: block_to_lba() maps it to 0 so
 * the pagecache zero-fills, while the chain itself keeps advancing. */
#define EXT4_HOLE_PBN       0UL

/* ===================================================================
 * In-memory state
 * =================================================================== */

typedef struct ext4_fs {
    const block_device_t* bdev;
    unsigned long  part_lba_offset;     /* partition base LBA on bdev        */
    unsigned int   block_size;          /* 1024 << s_log_block_size          */
    unsigned int   sectors_per_block;   /* block_size / bdev->sector_size    */
    unsigned int   inode_size;          /* s_inode_size                      */
    unsigned int   inodes_per_group;
    unsigned int   blocks_per_group;
    unsigned long  blocks_count;
    unsigned long  inodes_count;
    unsigned int   first_data_block;    /* 1 if block_size==1024 else 0      */
    unsigned int   desc_size;           /* 32 or 64                          */
    unsigned int   groups_count;
    unsigned int   first_ino;
    uint32_t       feature_incompat;
    uint32_t       feature_ro_compat;
    uint32_t       feature_compat;
    uint32_t       journal_inum;
    int            read_only;           /* mounted read-only (P1: always 1)  */
    int            meta_dirty;          /* GDT/superblock free counts pending writeback */
    struct ext4_group_desc *gdt;        /* cached group descriptor table     */
    ext4_super_block sb_copy;           /* cached superblock                 */
    /* VFS-layer superblock published to g_root_sb; sb.fs_private = this.    */
    vfs_superblock_t sb;
    /* PJ: journaled-writes (ordered mode) state.  j_enabled gates it all.   */
    int            j_enabled;           /* journal writes active this mount  */
    unsigned long  j_sb_pbn;            /* physical block of journal sblock  */
    unsigned long  j_first;             /* journal s_first (first log block) */
    unsigned long  j_maxlen;            /* journal s_maxlen (log block count)*/
    uint32_t       j_sequence;          /* next transaction sequence to use  */
    uint8_t        j_uuid[16];          /* journal uuid (descriptor tags)    */
    uint8_t       *j_sb_buf;            /* cached journal superblock (RMW)   */
} ext4_fs_t;

typedef struct ext4_file {
    vfs_file_t    vfs;                  /* MUST be first for casting         */
    ext4_fs_t*    fs;
    unsigned long ino;                  /* this file's inode number          */
    unsigned long size;                 /* file size in bytes                */
    unsigned int  mode;                 /* i_mode (type + perms)             */
    unsigned long pos;
    int           is_dir;
    unsigned long parent_ino;
    /* directory iteration cursor (byte offset into the directory file)      */
    unsigned long dir_pos;
    /* page-cache read-ahead state (mirrors fat32_file_t)                    */
    unsigned long ra_last_page;
    int           ra_seq_count;
    int           ra_pages;
    void*         inode;                /* ic_inode_t* from icache           */
} ext4_file_t;

/* ===================================================================
 * Public API (mount/probe + cache plumbing)
 * =================================================================== */

/* Probe + mount the ext4 filesystem on `bdev` into `out`.  Returns ST_OK on
 * success (a valid ext4 superblock was found and parsed). */
int ext4_mount(const block_device_t* bdev, ext4_fs_t* out);

/* Register a mounted ext4_fs_t as the VFS root (publishes g_root_sb and the
 * vfs_ops_t).  Returns ST_OK. */
int ext4_vfs_register_root(ext4_fs_t* fs);

/* Reentrant sleeping I/O mutex (same discipline as fat32_io_lock). */
void ext4_io_lock(void);
void ext4_io_unlock(void);
int  ext4_io_release_if_owner(uint64_t task_id);

/* Set the per-process current directory inode (used by chdir/relative paths). */
unsigned long ext4_get_cwd_ino(void);
void          ext4_set_cwd_ino(unsigned long ino);

/* The active ext4 root (non-NULL when ext4 is the mounted root). */
extern ext4_fs_t *g_ext4_fs;

/* Update a file's mtime (path-based).  mtime_nsec == 1073741823 means "now",
 * 1073741822 means "omit".  Returns ST_OK / ST_NOT_FOUND / ST_IO. */
int ext4_utimensat(const char *path, int64_t mtime_sec, long mtime_nsec);

/* Fill basic filesystem statistics for statfs(2).  Returns 0 on success. */
int ext4_get_statfs(unsigned long *f_bsize, unsigned long *f_blocks,
                    unsigned long *f_bfree, unsigned long *f_files,
                    unsigned long *f_ffree, unsigned long *f_namelen,
                    unsigned long *f_type);

/* Phase 3: symlinks / hard links / chmod / chown / lstat.  All return ST_OK
 * or an ST_* error (readlink returns the byte count). */
int ext4_symlink(const char *target, const char *linkpath);
int ext4_readlink(const char *path, char *buf, unsigned long bufsiz);
int ext4_link(const char *oldpath, const char *newpath);
int ext4_chmod(const char *path, unsigned mode);
int ext4_chown(const char *path, int uid, int gid);
int ext4_lstat(const char *path, struct kstat *st);
/* fd-based variants: ext4_file_ino() returns the inode behind an ext4 vfs_file
 * (0 if not ext4), to back fchmod/fchown. */
unsigned long ext4_file_ino(struct vfs_file *f);
int ext4_fchmod_ino(unsigned long ino, unsigned mode);
int ext4_fchown_ino(unsigned long ino, int uid, int gid);

#endif /* LIKEOS_EXT4_H */
