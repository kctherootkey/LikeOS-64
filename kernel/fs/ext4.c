// LikeOS-64 - ext4 filesystem driver
//
// Phase 1: read-only mount, extent + indirect block mapping, directory
// traversal, and integration with the generic page/inode caches.  Write
// support, symlinks, journaling and permission enforcement land in later
// phases (see plan).  Structurally this mirrors kernel/fs/fat32.c: a
// reentrant sleeping I/O mutex serialises operations, the data path runs
// through the shared pagecache, and the two vtables (vfs_ops_t for path/
// handle ops, vfs_sb_ops_t for the cache's block plumbing) are filled in.
//
// The block-chain integration uses self-describing block ids — see the
// big comment in include/kernel/fs/ext4.h.

#include <kernel/fs/ext4.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
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
#include <kernel/uapi/bug.h>

#define EXT4_MAX_SECTORS_PER_READ 128

/* On-disk layout is load-bearing: verify sizes/offsets at compile time. */
_Static_assert(sizeof(ext4_super_block) == 1024, "ext4_super_block must be 1024 bytes");
_Static_assert(sizeof(ext4_group_desc) == 64, "ext4_group_desc must be 64 bytes");
_Static_assert(sizeof(ext4_extent_header) == 12, "ext4_extent_header must be 12 bytes");
_Static_assert(sizeof(ext4_extent_idx) == 12, "ext4_extent_idx must be 12 bytes");
_Static_assert(sizeof(ext4_extent) == 12, "ext4_extent must be 12 bytes");
_Static_assert(__builtin_offsetof(ext4_super_block, s_magic) == 0x38, "s_magic offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_inode_size) == 0x58, "s_inode_size offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_feature_incompat) == 0x60, "s_feature_incompat offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_desc_size) == 0xFE, "s_desc_size offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_blocks_count_hi) == 0x150, "s_blocks_count_hi offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_links_count) == 0x1A, "i_links_count offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_flags) == 0x20, "i_flags offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_block) == 0x28, "i_block offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_size_high) == 0x6C, "i_size_high offset");
_Static_assert(__builtin_offsetof(ext4_group_desc, bg_inode_table_lo) == 8, "bg_inode_table_lo offset");
_Static_assert(sizeof(ext4_super_block) == 1024, "ext4_super_block must be 1024 bytes");
_Static_assert(__builtin_offsetof(ext4_super_block, s_checksum_type) == 0x175, "s_checksum_type offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_checksum_seed) == 0x270, "s_checksum_seed offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_checksum) == 0x3FC, "s_checksum offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_error_count) == 0x194, "s_error_count offset");
_Static_assert(__builtin_offsetof(ext4_group_desc, bg_checksum) == 0x1E, "bg_checksum offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_checksum_hi) == 0x82, "i_checksum_hi offset");

/* ===================================================================
 * P6: metadata_csum (crc32c, Castagnoli) — checksum machinery.
 *
 * Step 1 is read-side VERIFICATION only: compute the checksums the way the
 * reference does and WARN (once) if an on-disk value disagrees, so the crc32c
 * implementation + seed derivation can be proven against a real image before
 * any write-side code relies on them.  Nothing here rejects or mutates.
 * =================================================================== */

/* crc32c (reflected polynomial 0x82F63B78).  No implicit pre/post inversion —
 * the caller supplies the seed explicitly (matching the reference's usage). */
static uint32_t ext4_crc32c(uint32_t crc, const void *buf, unsigned long len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return crc;
}

/* Superblock checksum: crc32c(~0, sb, bytes-before-s_checksum). */
static uint32_t ext4_sb_csum(const ext4_super_block *sb)
{
    return ext4_crc32c(0xFFFFFFFFu, sb,
                       __builtin_offsetof(ext4_super_block, s_checksum));
}

/* Group-descriptor checksum (metadata_csum form): seed over the LE group
 * number, then the descriptor with the 2-byte bg_checksum field skipped,
 * truncated to 16 bits. */
static uint16_t ext4_gd_csum(const ext4_fs_t *fs, uint32_t group,
                             const ext4_group_desc *gd)
{
    const unsigned off = __builtin_offsetof(ext4_group_desc, bg_checksum);
    const uint16_t zero = 0;
    uint32_t le_group = group;                 /* x86 is little-endian */
    uint32_t crc = ext4_crc32c(fs->csum_seed, &le_group, sizeof(le_group));
    crc = ext4_crc32c(crc, gd, off);           /* up to bg_checksum         */
    crc = ext4_crc32c(crc, &zero, 2);          /* the zeroed bg_checksum     */
    if (fs->desc_size > off + 2)               /* rest after the csum field  */
        crc = ext4_crc32c(crc, (const uint8_t *)gd + off + 2,
                          fs->desc_size - (off + 2));
    return (uint16_t)(crc & 0xFFFF);
}

/* Inode checksum over the full on-disk inode bytes `raw` (length inode_size),
 * with the two embedded checksum fields treated as zero.  Returns the 32-bit
 * value; the low 16 bits go in l_i_checksum_lo (i_osd2+8), the high 16 in
 * i_checksum_hi (only when the inode is large enough to hold it). */
#define EXT4_INO_CSUM_LO_OFF   0x7C   /* l_i_checksum_lo within i_osd2[8]   */
#define EXT4_INO_CSUM_HI_OFF   0x82   /* i_checksum_hi                      */
#define EXT4_GOOD_OLD_ISIZE    128
static uint32_t ext4_inode_csum(const ext4_fs_t *fs, unsigned long ino,
                                const uint8_t *raw)
{
    uint32_t le_ino = (uint32_t)ino;
    uint32_t gen;
    mm_memcpy(&gen, raw + __builtin_offsetof(ext4_inode, i_generation), 4);
    const uint16_t zero = 0;
    uint32_t crc = ext4_crc32c(fs->csum_seed, &le_ino, sizeof(le_ino));
    crc = ext4_crc32c(crc, &gen, sizeof(gen));
    /* body up to l_i_checksum_lo, then a zeroed lo, then up to the old size */
    crc = ext4_crc32c(crc, raw, EXT4_INO_CSUM_LO_OFF);
    crc = ext4_crc32c(crc, &zero, 2);
    crc = ext4_crc32c(crc, raw + EXT4_INO_CSUM_LO_OFF + 2,
                      EXT4_GOOD_OLD_ISIZE - (EXT4_INO_CSUM_LO_OFF + 2));
    if (fs->inode_size > EXT4_GOOD_OLD_ISIZE) {
        uint16_t extra;
        mm_memcpy(&extra, raw + __builtin_offsetof(ext4_inode, i_extra_isize), 2);
        /* [old_size, i_checksum_hi) */
        crc = ext4_crc32c(crc, raw + EXT4_GOOD_OLD_ISIZE,
                          EXT4_INO_CSUM_HI_OFF - EXT4_GOOD_OLD_ISIZE);
        int has_hi = (extra >= (EXT4_INO_CSUM_HI_OFF + 2) - EXT4_GOOD_OLD_ISIZE);
        if (has_hi)
            crc = ext4_crc32c(crc, &zero, 2);   /* zeroed hi */
        else
            crc = ext4_crc32c(crc, raw + EXT4_INO_CSUM_HI_OFF, 2);
        crc = ext4_crc32c(crc, raw + EXT4_INO_CSUM_HI_OFF + 2,
                          fs->inode_size - (EXT4_INO_CSUM_HI_OFF + 2));
    }
    return crc;
}

/* ===================================================================
 * P6 Step 2: metadata_csum WRITE side — stamp the checksums computed above into
 * the on-disk metadata just before it is written.  Every helper is a no-op
 * unless the filesystem has metadata_csum, so a ^metadata_csum image is written
 * byte-for-byte as before.
 * =================================================================== */

/* Per-inode checksum seed (csum_seed folded with the inode number + its
 * generation).  Shared by the inode and directory-leaf checksums. */
static uint32_t ext4_inode_csum_seed(const ext4_fs_t *fs, unsigned long ino, uint32_t gen)
{
    uint32_t le_ino = (uint32_t)ino;
    uint32_t crc = ext4_crc32c(fs->csum_seed, &le_ino, sizeof(le_ino));
    return ext4_crc32c(crc, &gen, sizeof(gen));
}

/* Stamp the inode checksum into the full on-disk inode bytes `raw`
 * (fs->inode_size long) for inode `ino`.  Mirrors the field-presence rules of
 * ext4_inode_csum (the hi half only when the inode is large enough to hold it). */
static void ext4_inode_csum_set(const ext4_fs_t *fs, unsigned long ino, uint8_t *raw)
{
    if (!fs->has_metadata_csum) return;
    uint32_t csum = ext4_inode_csum(fs, ino, raw);
    *(uint16_t *)(raw + EXT4_INO_CSUM_LO_OFF) = (uint16_t)(csum & 0xFFFF);
    if (fs->inode_size > EXT4_GOOD_OLD_ISIZE) {
        uint16_t extra;
        mm_memcpy(&extra, raw + __builtin_offsetof(ext4_inode, i_extra_isize), 2);
        if (extra >= (EXT4_INO_CSUM_HI_OFF + 2) - EXT4_GOOD_OLD_ISIZE)
            *(uint16_t *)(raw + EXT4_INO_CSUM_HI_OFF) = (uint16_t)(csum >> 16);
    }
}

/* Lay out and checksum the 12-byte tail of a linear directory leaf block.  With
 * metadata_csum every dir block ends with a fake entry (inode=0, rec_len=12,
 * file_type=0xDE) whose last 4 bytes hold crc32c of the block before the tail.
 * `gen` is the directory inode's i_generation. */
#define EXT4_DIR_TAIL_SIZE 12
static void ext4_dir_csum_set(const ext4_fs_t *fs, unsigned long dir_ino,
                              uint32_t gen, uint8_t *blk)
{
    if (!fs->has_metadata_csum) return;
    uint8_t *tail = blk + fs->block_size - EXT4_DIR_TAIL_SIZE;
    tail[0] = tail[1] = tail[2] = tail[3] = 0;       /* det_reserved_zero1 (inode=0)  */
    tail[4] = EXT4_DIR_TAIL_SIZE; tail[5] = 0;       /* det_rec_len = 12              */
    tail[6] = 0;                                     /* det_reserved_zero2            */
    tail[7] = 0xDE;                                  /* det_reserved_ft (csum marker) */
    uint32_t seed = ext4_inode_csum_seed(fs, dir_ino, gen);
    uint32_t csum = ext4_crc32c(seed, blk, fs->block_size - EXT4_DIR_TAIL_SIZE);
    *(uint32_t *)(tail + 8) = csum;                  /* det_checksum                  */
}

/* Recompute the block / inode bitmap checksums (kept in the group descriptor)
 * after the bitmap buffer `bm` for group `g` is modified.  The descriptor is
 * flushed later by ext4_write_gd, which also recomputes bg_checksum over it.
 * The csum spans clusters_per_group/8 (== blocks_per_group/8, no bigalloc) /
 * (inodes_per_group+7)/8 bytes, exactly as the reference. */
static void ext4_block_bitmap_csum_set(ext4_fs_t *fs, unsigned g, const uint8_t *bm)
{
    if (!fs->has_metadata_csum) return;
    uint32_t csum = ext4_crc32c(fs->csum_seed, bm, fs->blocks_per_group / 8);
    fs->gdt[g].bg_block_bitmap_csum_lo = (uint16_t)(csum & 0xFFFF);
    if (fs->desc_size >= 64)
        fs->gdt[g].bg_block_bitmap_csum_hi = (uint16_t)(csum >> 16);
}
static void ext4_inode_bitmap_csum_set(ext4_fs_t *fs, unsigned g, const uint8_t *bm)
{
    if (!fs->has_metadata_csum) return;
    uint32_t csum = ext4_crc32c(fs->csum_seed, bm, (fs->inodes_per_group + 7) / 8);
    fs->gdt[g].bg_inode_bitmap_csum_lo = (uint16_t)(csum & 0xFFFF);
    if (fs->desc_size >= 64)
        fs->gdt[g].bg_inode_bitmap_csum_hi = (uint16_t)(csum >> 16);
}

/* ---- read-side VERIFICATION (enforcement): recompute and compare ---- */

/* bg_flags bits: an uninitialized group's bitmap is not laid out on disk yet, so
 * its stored csum is over the would-be-initialized image — skip verification. */
#define EXT4_BG_INODE_UNINIT 0x0001
#define EXT4_BG_BLOCK_UNINIT 0x0002

/* Verify a directory leaf's metadata_csum tail.  Returns 1 if OK, or if there is
 * nothing to verify: an fs without metadata_csum, or a block that does NOT end
 * with a dirent-checksum tail (the fake entry inode=0/rec_len=12/ft=0xDE).  The
 * latter guard means an htree dx node (different tail layout), which our linear
 * reader may encounter, is left to e2fsck instead of false-positiving. */
static int ext4_dir_csum_ok(const ext4_fs_t *fs, unsigned long dir_ino,
                            uint32_t gen, const uint8_t *blk)
{
    if (!fs->has_metadata_csum) return 1;
    const uint8_t *tail = blk + fs->block_size - EXT4_DIR_TAIL_SIZE;
    uint32_t tino; mm_memcpy(&tino, tail, 4);
    uint16_t trec; mm_memcpy(&trec, tail + 4, 2);
    if (tino != 0 || trec != EXT4_DIR_TAIL_SIZE || tail[6] != 0 || tail[7] != 0xDE)
        return 1;                                  /* not a dirent csum tail    */
    uint32_t want; mm_memcpy(&want, tail + 8, 4);
    uint32_t seed = ext4_inode_csum_seed(fs, dir_ino, gen);
    return ext4_crc32c(seed, blk, fs->block_size - EXT4_DIR_TAIL_SIZE) == want;
}

/* Verify a block/inode bitmap buffer against the csum kept in its group
 * descriptor.  Returns 1 if OK (no metadata_csum, or an uninitialized group). */
static int ext4_block_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g, const uint8_t *bm)
{
    if (!fs->has_metadata_csum) return 1;
    if (fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT) return 1;
    uint32_t csum = ext4_crc32c(fs->csum_seed, bm, fs->blocks_per_group / 8);
    uint32_t want = fs->gdt[g].bg_block_bitmap_csum_lo;
    if (fs->desc_size >= 64) want |= (uint32_t)fs->gdt[g].bg_block_bitmap_csum_hi << 16;
    else                     csum &= 0xFFFFu;
    return csum == want;
}
static int ext4_inode_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g, const uint8_t *bm)
{
    if (!fs->has_metadata_csum) return 1;
    if (fs->gdt[g].bg_flags & EXT4_BG_INODE_UNINIT) return 1;
    uint32_t csum = ext4_crc32c(fs->csum_seed, bm, (fs->inodes_per_group + 7) / 8);
    uint32_t want = fs->gdt[g].bg_inode_bitmap_csum_lo;
    if (fs->desc_size >= 64) want |= (uint32_t)fs->gdt[g].bg_inode_bitmap_csum_hi << 16;
    else                     csum &= 0xFFFFu;
    return csum == want;
}

/* Verify an EXTERNAL extent-tree block (depth>0 index/leaf node) against its
 * 4-byte et_checksum tail.  The tail sits right after eh_max entries
 * (EXT4_EXTENT_TAIL_OFFSET); the csum spans the block up to it, seeded by the
 * owning inode (ino+generation).  The inline depth-0 root in the inode body has
 * no tail (the inode csum covers it), so this is only for read-in child blocks.
 * Returns 1 if OK (no metadata_csum, or a malformed header left to other guards). */
static int ext4_extent_block_csum_ok(const ext4_fs_t *fs, unsigned long ino,
                                      uint32_t gen, const uint8_t *blk)
{
    if (!fs->has_metadata_csum) return 1;
    const ext4_extent_header *eh = (const ext4_extent_header *)blk;
    unsigned long off = sizeof(ext4_extent_header)
                      + (unsigned long)eh->eh_max * sizeof(ext4_extent);
    if (off + 4 > fs->block_size) return 1;     /* malformed eh_max — not ours  */
    uint32_t want; mm_memcpy(&want, blk + off, 4);
    uint32_t seed = ext4_inode_csum_seed(fs, ino, gen);
    return ext4_crc32c(seed, blk, off) == want;
}

/* Small byte compare (no mm_memcmp in the kernel).  Returns 0 if equal. */
static int ext4_memcmp(const void *a, const void *b, unsigned long n)
{
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    for (unsigned long i = 0; i < n; i++)
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    return 0;
}

/* The single mounted ext4 root (Phase 1 supports one ext4 filesystem). */
ext4_fs_t *g_ext4_fs = 0;

/* Refuse a mutating op up front when the fs is read-only / error-latched.  The
 * block-write chokepoints (ext4_write_block_direct / ext4_write_impl) already
 * prevent corruption; this just returns a clean -EROFS at entry instead of
 * letting an op do partial work and fail partway. */
static inline int ext4_is_ro(void) { return g_ext4_fs && g_ext4_fs->read_only; }

/* Global current-directory inode (mirrors FAT32's g_cwd_cluster approach). */
static unsigned long g_ext4_cwd_ino = EXT4_ROOT_INO;

unsigned long ext4_get_cwd_ino(void) { return g_ext4_cwd_ino; }
void ext4_set_cwd_ino(unsigned long ino) { g_ext4_cwd_ino = ino ? ino : EXT4_ROOT_INO; }

/* ===================================================================
 * PJ: journaled-writes transaction (ordered mode).
 *
 * A transaction spans one top-level operation: it begins on the outermost
 * ext4_io_lock() and commits on the matching outermost ext4_io_unlock().
 * While active, ext4_write_block() buffers the *structural* metadata it would
 * write (inode tables, directory blocks, allocation bitmaps) into s_txn
 * instead of writing it in place; commit then journals the whole set
 * atomically (descriptor + blocks + commit) and checkpoints it to the final
 * locations.  Data blocks are written in place during the op (ordered mode),
 * so committed metadata never references unwritten data.  Serialized entirely
 * by ext4_io_lock, so no extra locking is needed for s_txn.
 * =================================================================== */
static struct {
    int            active;
    unsigned       n, cap;
    unsigned long *blk;     /* final physical block number of each entry   */
    uint8_t      **data;    /* block_size bytes each (reused across txns)   */
} s_txn;

static inline int ext4_txn_active(void) { return s_txn.active; }
static int  ext4_write_block_direct(ext4_fs_t *fs, unsigned long pbn, const void *buf);
static void ext4_txn_flush(ext4_fs_t *fs);   /* defined with the journal code */
static void ext4_journal_clean(ext4_fs_t *fs);/* mark journal empty on sync     */
/* P6 enforcement: record a metadata-corruption error + apply the errors=
 * policy (remount-ro latch / panic / continue).  Defined after ext4_write_super. */
static void ext4_fs_error(ext4_fs_t *fs, const char *what, unsigned long ino);

/* Begin a transaction (called on the outermost ext4_io_lock acquire). */
static void ext4_txn_begin(void)
{
    if (!g_ext4_fs || !g_ext4_fs->j_enabled)
        return;
    s_txn.active = 1;
    s_txn.n      = 0;
}

/* Buffer (or overwrite) a metadata block in the running transaction. */
static void ext4_txn_capture(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
    for (unsigned i = 0; i < s_txn.n; i++)
        if (s_txn.blk[i] == pbn) {                /* re-dirtied in same op   */
            mm_memcpy(s_txn.data[i], buf, fs->block_size);
            return;
        }
    if (s_txn.n == s_txn.cap) {                   /* grow the pointer arrays */
        unsigned nc = s_txn.cap ? s_txn.cap * 2 : 16;
        unsigned long *nb = (unsigned long *)kalloc(nc * sizeof(unsigned long));
        uint8_t **nd = (uint8_t **)kalloc(nc * sizeof(uint8_t *));
        if (!nb || !nd) {                         /* OOM: degrade to direct  */
            if (nb) kfree(nb);
            if (nd) kfree(nd);
            WARN_ON_ONCE(1);
            ext4_write_block_direct(fs, pbn, buf);
            return;
        }
        for (unsigned i = 0; i < s_txn.n; i++) { nb[i] = s_txn.blk[i]; nd[i] = s_txn.data[i]; }
        for (unsigned i = s_txn.n; i < nc; i++) nd[i] = 0;   /* alloc on demand */
        if (s_txn.blk)  kfree(s_txn.blk);
        if (s_txn.data) kfree(s_txn.data);
        s_txn.blk = nb; s_txn.data = nd; s_txn.cap = nc;
    }
    if (!s_txn.data[s_txn.n])
        s_txn.data[s_txn.n] = (uint8_t *)kalloc(fs->block_size);
    if (!s_txn.data[s_txn.n]) { WARN_ON_ONCE(1); ext4_write_block_direct(fs, pbn, buf); return; }
    mm_memcpy(s_txn.data[s_txn.n], buf, fs->block_size);
    s_txn.blk[s_txn.n] = pbn;
    s_txn.n++;
}

/* Read-your-writes: serve a block from the running transaction if buffered. */
static int ext4_txn_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
    for (unsigned i = 0; i < s_txn.n; i++)
        if (s_txn.blk[i] == pbn) {
            mm_memcpy(buf, s_txn.data[i], fs->block_size);
            return 1;
        }
    return 0;
}

/* ===================================================================
 * P7: deferred-checkpoint circular journal log.
 *
 * Instead of checkpointing (writing committed metadata to its final location)
 * synchronously after every op — which cost a sync per op — committed
 * transactions accumulate as an "epoch" in the journal log and are checkpointed
 * lazily (when the log/pending set fills, on fsync/sync, or when an op frees a
 * block that still has a journal copy).  This drops the steady-state cost toward
 * ONE device sync per op (the commit), amortising the 2-sync checkpoint over the
 * whole epoch.
 *
 * s_ckpt is the in-memory authoritative copy of every metadata block written but
 * not yet checkpointed this epoch (latest content per block).  It serves two
 * roles: (1) read-your-writes — until checkpoint, a block's final disk location
 * still holds pre-epoch content, so reads must come from here; (2) the source
 * for the eventual checkpoint writeback.  Bounded by EXT4_CKPT_MAX_BLOCKS.
 *
 * Revoke records are NOT needed: the only journalled blocks our driver ever
 * frees are directory blocks (rmdir); freeing any block that still has a journal
 * copy sets s_force_ckpt, which checkpoints + empties the log right after the op,
 * so no stale journal copy of a freed/reused block can ever be replayed.
 * Serialized entirely by ext4_io_lock, like s_txn.
 * =================================================================== */
#define EXT4_CKPT_MAX_BLOCKS 256          /* pending unique-block cap (memory)  */
/* Max journal LOG blocks per epoch before forcing a checkpoint.  This bounds how
 * much a crash must replay (recovery writes ~one block per log block, and each is
 * a slow uncached device write) — i.e. it trades a little sync amortisation for a
 * fast boot after a crash.  Tunable: raise for fewer syncs/op (longer crash
 * recovery), lower for snappier recovery.  At 32: ~1.25 syncs/op, recovery of a
 * full epoch is ~30 writes.  (Steady state was 2 syncs/op before this; 3 before
 * that.)  Clean shutdowns checkpoint everything and never replay. */
#define EXT4_EPOCH_MAX_BLOCKS 32
static struct {
    unsigned       n, cap;
    unsigned long *blk;     /* final physical block number of each entry   */
    uint8_t      **data;    /* block_size bytes each                        */
} s_ckpt;
static unsigned long s_jhead;             /* next free journal log block        */
static int           s_epoch_open;        /* committed, un-checkpointed txns?    */
static uint32_t      s_epoch_seq;         /* sequence of the epoch's first txn   */
static int           s_force_ckpt;        /* a journalled block was freed        */
static void ext4_checkpoint(ext4_fs_t *fs);

/* Merge one block into the epoch pending set (latest content wins). */
static void ext4_ckpt_merge(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
    for (unsigned i = 0; i < s_ckpt.n; i++)
        if (s_ckpt.blk[i] == pbn) {
            mm_memcpy(s_ckpt.data[i], buf, fs->block_size);
            return;
        }
    if (s_ckpt.n == s_ckpt.cap) {
        unsigned nc = s_ckpt.cap ? s_ckpt.cap * 2 : 32;
        unsigned long *nb = (unsigned long *)kalloc(nc * sizeof(unsigned long));
        uint8_t **nd = (uint8_t **)kalloc(nc * sizeof(uint8_t *));
        if (!nb || !nd) {                 /* OOM: checkpoint now to drain, then
                                           * write this block direct as a fallback */
            if (nb) kfree(nb);
            if (nd) kfree(nd);
            WARN_ON_ONCE(1);
            ext4_write_block_direct(fs, pbn, buf);
            return;
        }
        for (unsigned i = 0; i < s_ckpt.n; i++) { nb[i] = s_ckpt.blk[i]; nd[i] = s_ckpt.data[i]; }
        for (unsigned i = s_ckpt.n; i < nc; i++) nd[i] = 0;
        if (s_ckpt.blk)  kfree(s_ckpt.blk);
        if (s_ckpt.data) kfree(s_ckpt.data);
        s_ckpt.blk = nb; s_ckpt.data = nd; s_ckpt.cap = nc;
    }
    if (!s_ckpt.data[s_ckpt.n])
        s_ckpt.data[s_ckpt.n] = (uint8_t *)kalloc(fs->block_size);
    if (!s_ckpt.data[s_ckpt.n]) { WARN_ON_ONCE(1); ext4_write_block_direct(fs, pbn, buf); return; }
    mm_memcpy(s_ckpt.data[s_ckpt.n], buf, fs->block_size);
    s_ckpt.blk[s_ckpt.n] = pbn;
    s_ckpt.n++;
}

/* Read-your-writes across the epoch: serve a committed-but-not-checkpointed
 * block (its final disk location is still stale until checkpoint). */
static int ext4_ckpt_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
    for (unsigned i = 0; i < s_ckpt.n; i++)
        if (s_ckpt.blk[i] == pbn) {
            mm_memcpy(buf, s_ckpt.data[i], fs->block_size);
            return 1;
        }
    return 0;
}

/* Does `pbn` still have an outstanding journal copy (current op or epoch)?  If
 * so, freeing it requires a checkpoint before the block can be reused, else a
 * crash could replay the stale copy over the block's new use. */
static int ext4_blk_journalled(unsigned long pbn)
{
    for (unsigned i = 0; i < s_txn.n;  i++) if (s_txn.blk[i]  == pbn) return 1;
    for (unsigned i = 0; i < s_ckpt.n; i++) if (s_ckpt.blk[i] == pbn) return 1;
    return 0;
}

/* ===================================================================
 * Reentrant sleeping I/O mutex (identical discipline to fat32_io_lock).
 * =================================================================== */
volatile int ext4_io_locked = 0;
volatile int ext4_io_depth  = 0;
volatile uint64_t ext4_io_owner = (uint64_t)-1;
static spinlock_t ext4_io_wait_lock = SPINLOCK_INIT("ext4_io_wait");

void ext4_io_lock(void)
{
    might_sleep();
    task_t *cur = sched_current();
    uint64_t my_id = cur ? cur->id : 0;
    while (1) {
        uint64_t flags;
        spin_lock_irqsave(&ext4_io_wait_lock, &flags);
        if (!ext4_io_locked) {
            ext4_io_locked = 1;
            ext4_io_owner  = my_id;
            ext4_io_depth  = 1;
            ext4_txn_begin();           /* outermost acquire starts a txn */
            spin_unlock_irqrestore(&ext4_io_wait_lock, flags);
            return;
        }
        if (ext4_io_owner == my_id) {
            ext4_io_depth++;
            spin_unlock_irqrestore(&ext4_io_wait_lock, flags);
            return;
        }
        if (cur) {
            cur->state = TASK_BLOCKED;
            cur->wait_channel = (void *)&ext4_io_locked;
        }
        spin_unlock_irqrestore(&ext4_io_wait_lock, flags);
        sched_schedule();
    }
}

void ext4_io_unlock(void)
{
    /* On the outermost release, commit the operation's transaction while the
     * lock is still held (depth==1, we are still the owner).  Done before the
     * spinlock since the flush sleeps on disk I/O.  Reads ext4_io_depth
     * lock-free: only the owning task mutates its own depth. */
    if (ext4_io_depth == 1 && s_txn.active)
        ext4_txn_flush(g_ext4_fs);

    uint64_t flags;
    spin_lock_irqsave(&ext4_io_wait_lock, &flags);
    WARN_ON(!ext4_io_locked);
    WARN_ON(ext4_io_depth <= 0);
    if (ext4_io_depth > 1) {
        ext4_io_depth--;
        spin_unlock_irqrestore(&ext4_io_wait_lock, flags);
        return;
    }
    ext4_io_locked = 0;
    ext4_io_owner  = (uint64_t)-1;
    ext4_io_depth  = 0;
    spin_unlock_irqrestore(&ext4_io_wait_lock, flags);
    sched_wake_channel((void *)&ext4_io_locked);
}

int ext4_io_release_if_owner(uint64_t task_id)
{
    uint64_t flags;
    int released = 0;
    spin_lock_irqsave(&ext4_io_wait_lock, &flags);
    if (ext4_io_locked && ext4_io_owner == task_id) {
        ext4_io_locked = 0;
        ext4_io_owner  = (uint64_t)-1;
        ext4_io_depth  = 0;
        /* The dead task may have been mid-operation: discard its uncommitted
         * transaction.  Nothing was journaled or checkpointed, so the fs stays
         * at its pre-operation state (atomicity).  Any committed epoch (s_ckpt +
         * log) is durable and stays — it is checkpointed/replayed normally.
         * Buffers are kept for reuse. */
        s_txn.active = 0;
        s_txn.n      = 0;
        s_force_ckpt = 0;        /* the freeing op was discarded with s_txn      */
        released = 1;
    }
    spin_unlock_irqrestore(&ext4_io_wait_lock, flags);
    if (released)
        sched_wake_channel((void *)&ext4_io_locked);
    return released;
}

/* ===================================================================
 * Low-level disk helpers
 * =================================================================== */

/* Read `count` 512-byte sectors at absolute `lba` (chunked for the USB DMA
 * limit, exactly like fat32's read_sectors). */
static int ext4_read_sectors(const block_device_t *bdev, unsigned long lba,
                             unsigned long count, void *buf)
{
    BUG_ON(bdev == NULL);
    BUG_ON(buf == NULL);
    might_sleep();
    unsigned long off = 0;
    unsigned ss = bdev->sector_size ? bdev->sector_size : 512;
    while (count > 0) {
        unsigned long chunk = (count > EXT4_MAX_SECTORS_PER_READ)
                            ? EXT4_MAX_SECTORS_PER_READ : count;
        int st = bdev->read((block_device_t *)bdev, lba, chunk,
                            (uint8_t *)buf + off);
        if (st != ST_OK)
            return st;
        lba   += chunk;
        off   += chunk * ss;
        count -= chunk;
    }
    return ST_OK;
}

/* ---- Metadata block cache ----
 * ext4 reads inode-table blocks, group-descriptor blocks, directory blocks
 * and extent/indirect nodes constantly, and the same block is re-read many
 * times in one operation (e.g. `ls -la` stats every entry, each re-reading
 * the parent dir block + the inode-table block).  Without caching, each is a
 * separate ~4KB USB transaction — painfully slow.  This small round-robin
 * cache keeps the hottest metadata blocks in RAM.  All ext4_read_block()
 * calls happen under the I/O mutex, so the cache needs no extra locking.
 * NOTE: read-only in Phase 1; Phase 2 must invalidate entries on writes. */
#define EXT4_MBC_ENTRIES 32
static struct {
    unsigned long pbn;      /* 0 = empty slot               */
    uint8_t      *data;     /* block_size bytes             */
} s_mbc[EXT4_MBC_ENTRIES];
static unsigned s_mbc_next;

static void ext4_mbc_invalidate(void)
{
    for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
        s_mbc[i].pbn = 0;
        if (s_mbc[i].data) { kfree(s_mbc[i].data); s_mbc[i].data = 0; }
    }
    s_mbc_next = 0;
}

/* Drop a single block from the metadata cache (on free, so a later reuse of
 * the same physical block never serves stale cached content). */
static void ext4_mbc_drop(unsigned long pbn)
{
    for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
        if (s_mbc[i].pbn == pbn) s_mbc[i].pbn = 0;
}

/* Read one filesystem block (block_size bytes) by physical block number,
 * serving from / populating the metadata block cache. */
static int ext4_read_block(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
    if (pbn == 0)
        return ST_INVALID;
    /* Read-your-writes: the current op's buffer wins, then the epoch's committed-
     * but-not-yet-checkpointed metadata (whose final disk location is still
     * stale), then the cache, then disk. */
    if (s_txn.active && ext4_txn_lookup(fs, pbn, buf))
        return ST_OK;
    if (s_ckpt.n && ext4_ckpt_lookup(fs, pbn, buf))
        return ST_OK;
    for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
        if (s_mbc[i].pbn == pbn && s_mbc[i].data) {
            mm_memcpy(buf, s_mbc[i].data, fs->block_size);
            return ST_OK;
        }
    }
    unsigned long lba = fs->part_lba_offset + pbn * fs->sectors_per_block;
    int st = ext4_read_sectors(fs->bdev, lba, fs->sectors_per_block, buf);
    if (st != ST_OK)
        return st;
    unsigned slot = s_mbc_next++ % EXT4_MBC_ENTRIES;
    if (!s_mbc[slot].data)
        s_mbc[slot].data = (uint8_t *)kalloc(fs->block_size);
    if (s_mbc[slot].data) {
        mm_memcpy(s_mbc[slot].data, buf, fs->block_size);
        s_mbc[slot].pbn = pbn;
    }
    return ST_OK;
}

/* ===================================================================
 * Group descriptor + inode reads
 * =================================================================== */

/* Load the entire group descriptor table into fs->gdt at mount time.  The
 * GDT is read on every inode access, so caching it in memory eliminates a
 * block read per inode lookup. */
static int ext4_load_gdt(ext4_fs_t *fs)
{
    if (fs->gdt) { kfree(fs->gdt); fs->gdt = 0; }
    fs->gdt = (ext4_group_desc *)kalloc(fs->groups_count * sizeof(ext4_group_desc));
    if (!fs->gdt)
        return ST_NOMEM;
    mm_memset(fs->gdt, 0, fs->groups_count * sizeof(ext4_group_desc));

    unsigned long gdt_start = (fs->first_data_block + 1);
    unsigned long per_block = fs->block_size / fs->desc_size;
    uint8_t *buf = (uint8_t *)kalloc(fs->block_size);
    if (!buf) { kfree(fs->gdt); fs->gdt = 0; return ST_NOMEM; }

    unsigned copy = fs->desc_size < sizeof(ext4_group_desc)
                  ? fs->desc_size : sizeof(ext4_group_desc);
    unsigned long cur_blk = (unsigned long)-1;
    for (unsigned int g = 0; g < fs->groups_count; g++) {
        unsigned long blk = gdt_start + g / per_block;
        unsigned long off = (g % per_block) * fs->desc_size;
        if (blk != cur_blk) {
            if (ext4_read_sectors(fs->bdev,
                    fs->part_lba_offset + blk * fs->sectors_per_block,
                    fs->sectors_per_block, buf) != ST_OK) {
                kfree(buf); kfree(fs->gdt); fs->gdt = 0; return ST_IO;
            }
            cur_blk = blk;
        }
        mm_memcpy(&fs->gdt[g], buf + off, copy);
    }
    kfree(buf);
    return ST_OK;
}

static int ext4_read_group_desc(ext4_fs_t *fs, unsigned int group,
                                ext4_group_desc *out)
{
    if (group >= fs->groups_count || !fs->gdt)
        return ST_INVALID;
    *out = fs->gdt[group];
    return ST_OK;
}

static unsigned long ext4_inode_table_block(ext4_fs_t *fs, const ext4_group_desc *gd)
{
    unsigned long lo = gd->bg_inode_table_lo;
    unsigned long hi = (fs->desc_size >= 64) ? gd->bg_inode_table_hi : 0;
    return lo | (hi << 32);
}

/* Read raw on-disk inode `ino` into `out`.  Also returns, when requested, the
 * physical block + byte offset of the inode (for O(1) writeback later). */
static int ext4_read_inode_loc(ext4_fs_t *fs, unsigned long ino,
                               ext4_inode *out,
                               unsigned long *out_block, unsigned *out_off)
{
    if (ino == 0 || ino > fs->inodes_count)
        return ST_INVALID;
    unsigned int group = (ino - 1) / fs->inodes_per_group;
    unsigned int index = (ino - 1) % fs->inodes_per_group;

    ext4_group_desc gd;
    int st = ext4_read_group_desc(fs, group, &gd);
    if (st != ST_OK)
        return st;

    unsigned long itbl = ext4_inode_table_block(fs, &gd);
    unsigned long byte = (unsigned long)index * fs->inode_size;
    unsigned long blk  = itbl + byte / fs->block_size;
    unsigned      off  = byte % fs->block_size;

    uint8_t *buf = (uint8_t *)kalloc(fs->block_size);
    if (!buf)
        return ST_NOMEM;
    st = ext4_read_block(fs, blk, buf);
    if (st != ST_OK) { kfree(buf); return st; }

    /* P6 enforcement: verify the inode's metadata_csum using the full on-disk
     * bytes still in `buf` (the struct copy below truncates them).  A mismatch
     * means the inode is corrupt — refuse to hand it back (ST_IO => -EIO) and
     * mark the filesystem errored, exactly like the reference's -EFSBADCRC. */
    if (fs->has_metadata_csum) {
        const uint8_t *rp = buf + off;
        uint32_t got = ext4_inode_csum(fs, ino, rp);
        uint16_t slo; mm_memcpy(&slo, rp + EXT4_INO_CSUM_LO_OFF, 2);
        uint32_t want = slo, mask = 0xFFFFu;
        if (fs->inode_size > EXT4_GOOD_OLD_ISIZE) {
            uint16_t extra;
            mm_memcpy(&extra, rp + __builtin_offsetof(ext4_inode, i_extra_isize), 2);
            if (extra >= (EXT4_INO_CSUM_HI_OFF + 2) - EXT4_GOOD_OLD_ISIZE) {
                uint16_t shi; mm_memcpy(&shi, rp + EXT4_INO_CSUM_HI_OFF, 2);
                want |= (uint32_t)shi << 16; mask = 0xFFFFFFFFu;
            }
        }
        if ((got & mask) != want) {
            kprintf("ext4: inode %lu metadata_csum mismatch "
                    "(disk 0x%x computed 0x%x)\n", ino, want, got & mask);
            kfree(buf);
            ext4_fs_error(fs, "inode checksum mismatch", ino);
            return ST_IO;
        }
    }

    mm_memset(out, 0, sizeof(*out));
    unsigned copy = fs->inode_size < sizeof(*out) ? fs->inode_size : sizeof(*out);
    mm_memcpy(out, buf + off, copy);
    kfree(buf);

    if (out_block) *out_block = blk;
    if (out_off)   *out_off   = off;
    return ST_OK;
}

/* Single-entry inode cache: the chain mapper resolves blocks sequentially for
 * one file at a time under the I/O lock, so caching the most-recently-read
 * inode collapses N inode reads per file down to 1. */
static unsigned long s_ic_ino = 0;
static ext4_inode    s_ic_inode;

static const ext4_inode *ext4_get_inode_cached(ext4_fs_t *fs, unsigned long ino)
{
    if (ino == s_ic_ino && s_ic_ino != 0)
        return &s_ic_inode;
    if (ext4_read_inode_loc(fs, ino, &s_ic_inode, 0, 0) != ST_OK) {
        s_ic_ino = 0;
        return 0;
    }
    s_ic_ino = ino;
    return &s_ic_inode;
}

static inline void ext4_inode_cache_drop(unsigned long ino)
{
    if (ino == s_ic_ino) s_ic_ino = 0;
}

static inline unsigned long ext4_inode_size(const ext4_inode *in)
{
    /* For regular files i_size_high holds the high 32 bits; for directories
     * the field is i_dir_acl and the directory size fits in 32 bits. */
    if ((in->i_mode & S_IFMT) == S_IFREG)
        return (unsigned long)in->i_size_lo | ((unsigned long)in->i_size_high << 32);
    return in->i_size_lo;
}

/* ===================================================================
 * Logical -> physical block mapping (extents + classic indirect)
 * =================================================================== */

/* Walk an extent tree to map logical block `lidx` -> physical block.
 * Returns 0 for a hole / past-EOF / error. */
static unsigned long ext4_extent_map(ext4_fs_t *fs, unsigned long ino,
                                     const ext4_inode *in, unsigned long lidx)
{
    const uint8_t *node = in->i_block;     /* root lives inline in the inode */
    uint8_t *heap = 0;
    unsigned long result = 0;

    for (int depth_guard = 0; depth_guard < 6; depth_guard++) {
        const ext4_extent_header *eh = (const ext4_extent_header *)node;
        if (eh->eh_magic != EXT4_EXT_MAGIC) {
            WARN_ON_ONCE(1);  /* corrupt extent header magic */
            break;
        }
        unsigned entries = eh->eh_entries;
        if (eh->eh_depth == 0) {
            const ext4_extent *ex = (const ext4_extent *)(eh + 1);
            for (unsigned i = 0; i < entries; i++) {
                unsigned long start = ex[i].ee_block;
                unsigned len = ex[i].ee_len;
                if (len > 32768) len -= 32768;   /* uninitialized extent */
                if (lidx >= start && lidx < start + len) {
                    unsigned long phys = (unsigned long)ex[i].ee_start_lo
                                       | ((unsigned long)ex[i].ee_start_hi << 32);
                    result = phys + (lidx - start);
                    goto done;
                }
            }
            break;  /* no covering extent: hole / EOF */
        } else {
            const ext4_extent_idx *ix = (const ext4_extent_idx *)(eh + 1);
            int chosen = -1;
            for (unsigned i = 0; i < entries; i++) {
                if (ix[i].ei_block <= lidx)
                    chosen = (int)i;
                else
                    break;
            }
            if (chosen < 0)
                break;
            unsigned long child = (unsigned long)ix[chosen].ei_leaf_lo
                                | ((unsigned long)ix[chosen].ei_leaf_hi << 32);
            if (!heap) {
                heap = (uint8_t *)kalloc(fs->block_size);
                if (!heap)
                    break;
            }
            if (ext4_read_block(fs, child, heap) != ST_OK)
                break;
            if (!ext4_extent_block_csum_ok(fs, ino, in->i_generation, heap)) {
                ext4_fs_error(fs, "extent block checksum mismatch", ino);
                break;   /* corrupt index/leaf node: treat as unmapped */
            }
            node = heap;   /* descend */
        }
    }
done:
    if (heap)
        kfree(heap);
    return result;
}

/* Classic ext2/3 indirect block mapping (for inodes without EXTENTS_FL). */
static unsigned long ext4_indirect_map(ext4_fs_t *fs, const ext4_inode *in,
                                       unsigned long lidx)
{
    const uint32_t *direct = (const uint32_t *)in->i_block;
    unsigned long ppb = fs->block_size / 4;

    if (lidx < EXT4_NDIR_BLOCKS)
        return direct[lidx];

    lidx -= EXT4_NDIR_BLOCKS;
    uint32_t *buf = (uint32_t *)kalloc(fs->block_size);
    if (!buf)
        return 0;
    unsigned long result = 0;

    if (lidx < ppb) {
        unsigned long ind = direct[EXT4_IND_BLOCK];
        if (ind && ext4_read_block(fs, ind, buf) == ST_OK)
            result = buf[lidx];
        goto out;
    }
    lidx -= ppb;
    if (lidx < ppb * ppb) {
        unsigned long dind = direct[EXT4_DIND_BLOCK];
        if (dind && ext4_read_block(fs, dind, buf) == ST_OK) {
            unsigned long ind = buf[lidx / ppb];
            if (ind && ext4_read_block(fs, ind, buf) == ST_OK)
                result = buf[lidx % ppb];
        }
        goto out;
    }
    lidx -= ppb * ppb;
    {
        unsigned long tind = direct[EXT4_TIND_BLOCK];
        if (tind && ext4_read_block(fs, tind, buf) == ST_OK) {
            unsigned long dind = buf[lidx / (ppb * ppb)];
            unsigned long rem  = lidx % (ppb * ppb);
            if (dind && ext4_read_block(fs, dind, buf) == ST_OK) {
                unsigned long ind = buf[rem / ppb];
                if (ind && ext4_read_block(fs, ind, buf) == ST_OK)
                    result = buf[rem % ppb];
            }
        }
    }
out:
    kfree(buf);
    return result;
}

/* Map (inode, logical block index) -> physical block number (0 = hole/EOF). */
static unsigned long ext4_block_map(ext4_fs_t *fs, unsigned long ino,
                                    unsigned long lidx)
{
    const ext4_inode *in = ext4_get_inode_cached(fs, ino);
    if (!in)
        return 0;
    if (in->i_flags & EXT4_INODE_EXTENTS_FL)
        return ext4_extent_map(fs, ino, in, lidx);
    return ext4_indirect_map(fs, in, lidx);
}

/* ===================================================================
 * vfs_sb_ops_t — the generic cache <-> ext4 block plumbing.
 *
 * Chain ids are self-describing: EXT4_BID_ENC(ino, logical_index).  These
 * ops are therefore pure functions of the id (plus the on-disk extent map),
 * needing no per-FS reverse-map state.
 * =================================================================== */

static int ext4_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *ino);

static unsigned long ext4_sb_block_size(vfs_superblock_t *sb)
{ return ((ext4_fs_t *)sb->fs_private)->block_size; }

static const block_device_t *ext4_sb_bdev(vfs_superblock_t *sb)
{ return ((ext4_fs_t *)sb->fs_private)->bdev; }

static unsigned long ext4_sb_sector_size(vfs_superblock_t *sb)
{ return ((ext4_fs_t *)sb->fs_private)->bdev->sector_size; }

static unsigned long ext4_sb_block_to_lba(vfs_superblock_t *sb, unsigned long bid)
{
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    if (!EXT4_BID_IS(bid))
        return 0;
    unsigned long pbn = ext4_block_map(fs, EXT4_BID_INO(bid), EXT4_BID_LIDX(bid));
    if (pbn == EXT4_HOLE_PBN)
        return 0;   /* hole: pagecache zero-fills */
    return fs->part_lba_offset + pbn * fs->sectors_per_block;
}

static unsigned long ext4_sb_end_of_chain(vfs_superblock_t *sb)
{ (void)sb; return EXT4_EOC_MARKER; }

static unsigned long ext4_sb_next_block(vfs_superblock_t *sb, unsigned long bid)
{
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    if (!EXT4_BID_IS(bid))
        return EXT4_BID_EOC;
    unsigned long ino  = EXT4_BID_INO(bid);
    unsigned long lidx = EXT4_BID_LIDX(bid);
    /* Stop at EOF.  For Phase 1 (non-sparse images) a 0 mapping means EOF;
     * proper mid-file hole handling arrives with write support. */
    if (ext4_block_map(fs, ino, lidx + 1) == 0)
        return EXT4_BID_EOC;
    return EXT4_BID_ENC(ino, lidx + 1);
}

static void ext4_sb_lock_io(vfs_superblock_t *sb)   { (void)sb; ext4_io_lock(); }
static void ext4_sb_unlock_io(vfs_superblock_t *sb) { (void)sb; ext4_io_unlock(); }

static unsigned long ext4_sb_reserved_meta_block(vfs_superblock_t *sb)
{ (void)sb; return 0; }

/* Persist a dirty cached inode's size back to disk (called by icache_flush).
 * Defined out-of-line below where the write helpers are available. */
static int ext4_write_inode_struct(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in);
static int ext4_read_inode_loc(ext4_fs_t *fs, unsigned long ino, ext4_inode *out,
                               unsigned long *out_block, unsigned *out_off);

static int ext4_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *inode)
{
    if (!inode) return 0;
    ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
    unsigned long ino = EXT4_BID_INO(inode->start_cluster);
    if (ino == 0 || ino > fs->inodes_count) return 0;
    ext4_inode in;
    if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK) return -1;
    in.i_size_lo = (uint32_t)inode->size;
    if ((in.i_mode & S_IFMT) == S_IFREG)
        in.i_size_high = (uint32_t)(inode->size >> 32);
    s_ic_ino = 0;
    ext4_write_inode_struct(fs, ino, &in);
    return 0;
}

static const vfs_sb_ops_t ext4_sb_ops = {
    .block_size          = ext4_sb_block_size,
    .bdev                = ext4_sb_bdev,
    .sector_size         = ext4_sb_sector_size,
    .block_to_lba        = ext4_sb_block_to_lba,
    .end_of_chain_marker = ext4_sb_end_of_chain,
    .next_block          = ext4_sb_next_block,
    .write_inode         = ext4_sb_write_inode,
    .lock_io             = ext4_sb_lock_io,
    .unlock_io           = ext4_sb_unlock_io,
    .reserved_meta_block = ext4_sb_reserved_meta_block,
};

static void ext4_sb_attach(ext4_fs_t *fs)
{
    fs->sb.ops        = &ext4_sb_ops;
    fs->sb.fs_private = fs;
    g_root_sb         = &fs->sb;
}

/* ===================================================================
 * Directory traversal + path resolution
 * =================================================================== */

/* Look up `name` (length name_len) in directory inode `dir_ino`.
 * On success returns ST_OK and sets *out_ino (and *out_ftype if non-NULL). */
static int ext4_dir_lookup(ext4_fs_t *fs, unsigned long dir_ino,
                           const char *name, unsigned name_len,
                           unsigned long *out_ino, unsigned *out_ftype)
{
    const ext4_inode *din = ext4_get_inode_cached(fs, dir_ino);
    if (!din || (din->i_mode & S_IFMT) != S_IFDIR)
        return ST_NOT_FOUND;
    unsigned long dsize = ext4_inode_size(din);
    unsigned long nblocks = (dsize + fs->block_size - 1) / fs->block_size;
    uint32_t gen = din->i_generation;   /* for the leaf csum (din may be evicted below) */

    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk)
        return ST_NOMEM;

    for (unsigned long b = 0; b < nblocks; b++) {
        unsigned long pbn = ext4_block_map(fs, dir_ino, b);
        if (pbn == 0)
            continue;   /* sparse dir block (rare) */
        if (ext4_read_block(fs, pbn, blk) != ST_OK)
            continue;
        if (!ext4_dir_csum_ok(fs, dir_ino, gen, blk)) {
            kfree(blk);
            ext4_fs_error(fs, "directory leaf checksum mismatch", dir_ino);
            return ST_IO;
        }
        unsigned off = 0;
        while (off + 8 <= fs->block_size) {
            ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
            unsigned rec = de->rec_len;
            if (rec < 8 || off + rec > fs->block_size) {
                WARN_ON_ONCE(1);  /* bad dir rec_len — stop scanning this block */
                break;
            }
            if (de->inode != 0 && de->name_len == name_len &&
                ext4_memcmp(de->name, name, name_len) == 0) {
                *out_ino = de->inode;
                if (out_ftype) *out_ftype = de->file_type;
                kfree(blk);
                return ST_OK;
            }
            off += rec;
        }
    }
    kfree(blk);
    return ST_NOT_FOUND;
}

/* Read a symlink's target into buf (NUL-terminated).  Fast symlinks (<60B,
 * i_blocks==0) store the target inline in i_block; longer "slow" symlinks
 * store it in data block 0.  Returns target length, or -1 on error. */
static int ext4_read_symlink_target(ext4_fs_t *fs, unsigned long ino,
                                    const ext4_inode *in, char *buf, unsigned cap)
{
    unsigned long len = in->i_size_lo;
    if (len == 0 || len >= cap) { if (len >= cap) len = cap - 1; else return -1; }
    if (in->i_blocks_lo == 0) {
        mm_memcpy(buf, in->i_block, len);
    } else {
        unsigned long pbn = ext4_block_map(fs, ino, 0);
        if (pbn == 0) return -1;
        uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
        if (!blk) return -1;
        if (ext4_read_sectors(fs->bdev,
                fs->part_lba_offset + pbn * fs->sectors_per_block,
                fs->sectors_per_block, blk) != ST_OK) { kfree(blk); return -1; }
        mm_memcpy(buf, blk, len);
        kfree(blk);
    }
    buf[len] = '\0';
    return (int)len;
}

/* Resolve a path to an inode, following symlinks.  `start_ino` is the dir for
 * relative paths; `follow_final` controls whether a symlink in the LAST
 * component is followed (stat yes, lstat/readlink no).  `depth` guards loops. */
static int ext4_resolve_ex(ext4_fs_t *fs, unsigned long start_ino,
                           const char *path, int follow_final,
                           unsigned long *out_ino, int depth)
{
    if (!path || !out_ino) return ST_INVALID;
    if (depth > 12) return ST_INVALID;            /* ELOOP guard            */
    unsigned long cur = (path[0] == '/') ? EXT4_ROOT_INO : start_ino;
    const char *p = path;
    while (*p == '/') p++;

    char comp[256];
    while (*p) {
        unsigned ci = 0;
        while (*p && *p != '/') { if (ci < sizeof(comp) - 1) comp[ci++] = *p; p++; }
        comp[ci] = '\0';
        while (*p == '/') p++;
        int is_last = (*p == '\0');

        if (ci == 0 || (ci == 1 && comp[0] == '.')) continue;
        if (ci == 2 && comp[0] == '.' && comp[1] == '.') {
            unsigned long parent;
            if (ext4_dir_lookup(fs, cur, "..", 2, &parent, 0) == ST_OK) cur = parent;
            continue;
        }
        unsigned long child; unsigned ftype = 0;
        int lr = ext4_dir_lookup(fs, cur, comp, ci, &child, &ftype);
        if (lr != ST_OK)                          /* a corrupt directory leaf
                                                     returns ST_IO; propagate it
                                                     rather than masking ENOENT  */
            return lr;

        const ext4_inode *cin = ext4_get_inode_cached(fs, child);
        if (!cin) return ST_IO;
        if ((cin->i_mode & S_IFMT) == S_IFLNK && (!is_last || follow_final)) {
            char target[256];
            int tl = ext4_read_symlink_target(fs, child, cin, target, sizeof(target));
            if (tl <= 0) return ST_IO;
            unsigned long linked;
            int r = ext4_resolve_ex(fs, cur, target, 1, &linked, depth + 1);
            if (r != ST_OK) return r;
            cur = linked;
        } else {
            cur = child;
        }
        if (!is_last) {
            const ext4_inode *d = ext4_get_inode_cached(fs, cur);
            if (!d) return ST_IO;                 /* unreadable/corrupt component */
            if ((d->i_mode & S_IFMT) != S_IFDIR) return ST_NOT_FOUND;
        }
    }
    *out_ino = cur;
    return ST_OK;
}

/* Default resolver: relative to cwd, follows a final symlink. */
static int ext4_resolve(ext4_fs_t *fs, const char *path, unsigned long *out_ino)
{
    return ext4_resolve_ex(fs, g_ext4_cwd_ino, path, 1, out_ino, 0);
}

/* ===================================================================
 * VFS operations (read-only subset for Phase 1)
 * =================================================================== */

static const vfs_ops_t ext4_vfs_ops;   /* forward */

/* Phase-2 write helpers (defined further below; used by open/truncate). */
static int  ext4_resolve_parent(ext4_fs_t *fs, const char *path,
                                unsigned long *parent_ino, char *name_out, unsigned cap);
static unsigned long ext4_alloc_inode(ext4_fs_t *fs, unsigned long parent_ino, int is_dir);
static int  ext4_create_inode(ext4_fs_t *fs, unsigned long ino, unsigned mode,
                              unsigned uid, unsigned gid);
static void ext4_free_inode(ext4_fs_t *fs, unsigned long ino, int is_dir);
static void ext4_init_owner(unsigned puid, unsigned pgid, unsigned pmode, int is_dir,
                            unsigned *uid, unsigned *gid, unsigned *mode);
static int  ext4_dir_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
                         unsigned name_len, unsigned long child_ino, unsigned ftype);
static void ext4_free_blocks_from(ext4_fs_t *fs, ext4_inode *in, unsigned long from);
static void ext4_flush_meta(ext4_fs_t *fs);

static int ext4_open_impl(const char *path, int flags, vfs_file_t **out)
{
    if (!out || !path || !path[0] || !g_ext4_fs)
        return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;

    unsigned long ino = 0;
    int rr = ext4_resolve(fs, path, &ino);
    if (rr != ST_OK) {
        /* A genuine I/O / corruption error (e.g. a bad inode metadata_csum)
         * must surface as -EIO, not be masked as "not found" — and must not
         * trigger O_CREAT, since the path may exist but simply be unreadable. */
        if (rr != ST_NOT_FOUND)
            return rr;
        /* Genuinely absent — create it if requested. */
        if (!(flags & O_CREAT))
            return ST_NOT_FOUND;
        unsigned long parent = 0;
        char nm[256];
        if (ext4_resolve_parent(fs, path, &parent, nm, sizeof(nm)) != ST_OK)
            return ST_NOT_FOUND;
        unsigned nl = 0; while (nm[nl]) nl++;
        /* Owner/group of the new file per the reference (the creating process). */
        unsigned puid = 0, pgid = 0, pmode = 0;
        ext4_inode pin0;
        if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
            puid = pin0.i_uid; pgid = pin0.i_gid; pmode = pin0.i_mode;
        }
        unsigned nuid = puid, ngid = pgid, nmode = S_IFREG | 0644;
        ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);
        unsigned long newino = ext4_alloc_inode(fs, parent, 0);
        if (newino == 0) return ST_NOMEM;
        if (ext4_create_inode(fs, newino, nmode, nuid, ngid) != ST_OK) {
            ext4_free_inode(fs, newino, 0); return ST_IO;
        }
        if (ext4_dir_add(fs, parent, nm, nl, newino, EXT4_FT_REG_FILE) != ST_OK) {
            ext4_free_inode(fs, newino, 0); return ST_IO;
        }
        s_ic_ino = 0;
        ino = newino;
    }

    ext4_inode in;
    if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
        return ST_IO;

    unsigned mode = in.i_mode;

    /* O_TRUNC on an existing regular file: discard its data. */
    if ((flags & O_TRUNC) && (mode & S_IFMT) == S_IFREG && ext4_inode_size(&in) > 0
        && !ext4_is_ro()) {
        ext4_free_blocks_from(fs, &in, 0);
        in.i_size_lo = 0; in.i_size_high = 0;
        in.i_mtime = in.i_ctime = (uint32_t)timer_get_epoch();
        s_ic_ino = 0;
        ext4_write_inode_struct(fs, ino, &in);
        pagecache_invalidate_file(EXT4_BID_ENC(ino, 0));
        icache_chain_invalidate(EXT4_BID_ENC(ino, 0));
    }

    /* Flush any deferred GDT/superblock updates from create/truncate alloc. */
    if (fs->meta_dirty) ext4_flush_meta(fs);

    ext4_file_t *ef = (ext4_file_t *)kalloc(sizeof(ext4_file_t));
    if (!ef)
        return ST_NOMEM;
    mm_memset(ef, 0, sizeof(*ef));
    ef->fs   = fs;
    ef->ino  = ino;
    ef->mode = mode;
    ef->size = ext4_inode_size(&in);
    ef->pos  = (flags & O_APPEND) ? ef->size : 0;
    ef->is_dir = ((mode & S_IFMT) == S_IFDIR);
    ef->vfs.ops = &ext4_vfs_ops;
    ef->vfs.fs_private = ef;

    if (!ef->is_dir && (mode & S_IFMT) == S_IFREG) {
        /* Attach an icache entry keyed by the encoded chain id so the
         * generic chain cache hangs off the same inode. */
        ef->inode = icache_get(EXT4_BID_ENC(ino, 0), ef->size, mode,
                               0, 0, 0, 0, 0);
    }

    *out = &ef->vfs;
    return ST_OK;
}

static int ext4_stat_fill(ext4_fs_t *fs, unsigned long ino, struct kstat *st)
{
    ext4_inode in;
    if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
        return ST_IO;
    mm_memset(st, 0, sizeof(*st));
    st->st_dev   = 0;
    st->st_ino   = ino;
    st->st_mode  = in.i_mode;
    st->st_nlink = in.i_links_count;
    st->st_uid   = in.i_uid;
    st->st_gid   = in.i_gid;
    st->st_size  = ext4_inode_size(&in);
    st->st_blksize = fs->block_size;
    st->st_blocks  = in.i_blocks_lo;   /* in 512-byte units */
    st->st_atime = in.i_atime;
    st->st_mtime = in.i_mtime;
    st->st_ctime = in.i_ctime;
    return ST_OK;
}

static int ext4_stat_vfs_impl(const char *path, struct kstat *st)
{
    if (!st || !path || !g_ext4_fs)
        return ST_INVALID;
    unsigned long ino = 0;
    int rr = ext4_resolve(g_ext4_fs, path, &ino);
    if (rr != ST_OK)                              /* propagate ST_IO, not ENOENT */
        return rr;
    return ext4_stat_fill(g_ext4_fs, ino, st);
}

static long ext4_read_impl(vfs_file_t *f, void *buf, long bytes)
{
    if (!f || !buf)
        return ST_INVALID;
    ext4_file_t *ef = (ext4_file_t *)f->fs_private;
    if (!ef || ef->is_dir)
        return ef && ef->is_dir ? -EISDIR : ST_INVALID;
    if (bytes < 0)
        return ST_INVALID;
    if (ef->pos >= ef->size)
        return 0;
    if ((unsigned long)bytes > ef->size - ef->pos)
        bytes = (long)(ef->size - ef->pos);

    unsigned long remaining = (unsigned long)bytes;
    unsigned long copied = 0;
    unsigned long chain_id = EXT4_BID_ENC(ef->ino, 0);

    smap_disable();
    while (remaining) {
        unsigned long page_idx = ef->pos / PAGE_SIZE;
        unsigned page_off = ef->pos % PAGE_SIZE;
        unsigned avail = PAGE_SIZE - page_off;
        unsigned chunk = (remaining < avail) ? (unsigned)remaining : avail;

        pc_page_t *pg = pagecache_get(chain_id, page_idx, ef->size,
                                      &ef->fs->sb, chain_id);
        if (!pg || !pg->data) {
            smap_enable();
            return copied ? (long)copied : ST_IO;
        }
        mm_memcpy((uint8_t *)buf + copied, pg->data + page_off, chunk);

        pc_readahead_t ra;
        ra.last_page_index  = ef->ra_last_page;
        ra.sequential_count = ef->ra_seq_count;
        ra.ra_pages         = ef->ra_pages;
        pagecache_readahead(&ra, chain_id, page_idx, ef->size,
                            &ef->fs->sb, chain_id);
        ef->ra_last_page = ra.last_page_index;
        ef->ra_seq_count = ra.sequential_count;
        ef->ra_pages     = ra.ra_pages;

        ef->pos   += chunk;
        copied    += chunk;
        remaining -= chunk;
    }
    smap_enable();
    return (long)copied;
}

static long ext4_seek_impl(vfs_file_t *f, long offset, int whence)
{
    ext4_file_t *ef = (ext4_file_t *)f->fs_private;
    if (!ef)
        return ST_INVALID;
    long base;
    switch (whence) {
        case 0:  base = 0; break;                 /* SEEK_SET */
        case 1:  base = (long)ef->pos; break;     /* SEEK_CUR */
        case 2:  base = (long)ef->size; break;    /* SEEK_END */
        default: return ST_INVALID;
    }
    long np = base + offset;
    if (np < 0)
        return ST_INVALID;
    ef->pos = (unsigned long)np;
    return np;
}

static unsigned ext4_ft_to_dt(unsigned ft)
{
    switch (ft) {
        case EXT4_FT_REG_FILE: return DT_REG;
        case EXT4_FT_DIR:      return DT_DIR;
        case EXT4_FT_CHRDEV:   return DT_CHR;
        case EXT4_FT_BLKDEV:   return DT_BLK;
        case EXT4_FT_FIFO:     return DT_FIFO;
        case EXT4_FT_SOCK:     return DT_SOCK;
        case EXT4_FT_SYMLINK:  return DT_LNK;
        default:               return DT_UNKNOWN;
    }
}

static long ext4_readdir_impl(vfs_file_t *f, void *buf, long bytes)
{
    if (!f || !buf || bytes <= 0)
        return ST_INVALID;
    ext4_file_t *ef = (ext4_file_t *)f->fs_private;
    if (!ef || !ef->is_dir)
        return -ENOTDIR;
    ext4_fs_t *fs = ef->fs;

    const ext4_inode *din = ext4_get_inode_cached(fs, ef->ino);
    if (!din)
        return ST_IO;
    unsigned long dsize = ext4_inode_size(din);
    uint32_t gen = din->i_generation;   /* for the leaf csum (din may be evicted below) */

    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk)
        return ST_NOMEM;

    unsigned out_off = 0;
    while (ef->dir_pos < dsize) {
        unsigned long b   = ef->dir_pos / fs->block_size;
        unsigned in_blk   = ef->dir_pos % fs->block_size;
        unsigned long pbn = ext4_block_map(fs, ef->ino, b);
        if (pbn == 0) {
            ef->dir_pos = (b + 1) * fs->block_size;   /* skip sparse block */
            continue;
        }
        if (ext4_read_block(fs, pbn, blk) != ST_OK) {
            ef->dir_pos = (b + 1) * fs->block_size;
            continue;
        }
        if (!ext4_dir_csum_ok(fs, ef->ino, gen, blk)) {
            kfree(blk);
            ext4_fs_error(fs, "directory leaf checksum mismatch", ef->ino);
            return ST_IO;
        }
        int block_done = 0;
        while (in_blk + 8 <= fs->block_size) {
            ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + in_blk);
            unsigned rec = de->rec_len;
            if (rec < 8 || in_blk + rec > fs->block_size) {
                WARN_ON_ONCE(1);
                block_done = 1;
                break;
            }
            if (de->inode != 0 && de->name_len != 0) {
                unsigned namelen = de->name_len;
                unsigned reclen = (19 + namelen + 1 + 7) & ~7u;  /* align 8 */
                if (out_off + reclen > (unsigned)bytes) {
                    /* Output buffer full; resume here next call. */
                    kfree(blk);
                    return out_off ? (long)out_off : -EINVAL;
                }
                /* SMAP-aware write to the user buffer: without STAC these
                 * supervisor writes to a user page fault on SMAP-capable CPUs
                 * (real hw / VMware), and since the page is present+writable
                 * the fault handler finds nothing to fix and re-faults forever
                 * (silent hang).  Matches fat32_write_dirent64. */
                struct linux_dirent64 *ld =
                    (struct linux_dirent64 *)((uint8_t *)buf + out_off);
                smap_disable();
                ld->d_ino    = de->inode;
                ld->d_off    = (int64_t)(ef->dir_pos + rec);
                ld->d_reclen = (uint16_t)reclen;
                ld->d_type   = (uint8_t)ext4_ft_to_dt(de->file_type);
                mm_memcpy(ld->d_name, de->name, namelen);
                ld->d_name[namelen] = '\0';
                smap_enable();
                out_off += reclen;
            }
            in_blk      += rec;
            ef->dir_pos += rec;
        }
        if (block_done) {
            /* Advance to the next block boundary. */
            ef->dir_pos = (b + 1) * fs->block_size;
        }
    }
    kfree(blk);
    return (long)out_off;
}

static int ext4_close_impl(vfs_file_t *f)
{
    if (!f)
        return ST_INVALID;
    ext4_file_t *ef = (ext4_file_t *)f->fs_private;
    if (ef) {
        if (ef->inode)
            icache_unref((ic_inode_t *)ef->inode);
        kfree(ef);
    }
    return ST_OK;
}

static int ext4_chdir_impl(const char *path)
{
    if (!path || !g_ext4_fs)
        return ST_INVALID;
    unsigned long ino = 0;
    int rr = ext4_resolve(g_ext4_fs, path, &ino);
    if (rr != ST_OK)                              /* propagate ST_IO, not ENOENT */
        return rr;
    const ext4_inode *in = ext4_get_inode_cached(g_ext4_fs, ino);
    if (!in || (in->i_mode & S_IFMT) != S_IFDIR)
        return -ENOTDIR;
    g_ext4_cwd_ino = ino;
    return ST_OK;
}

/* ===================================================================
 * Phase 2: write support — allocation, metadata writeback, file lifecycle.
 *
 * Metadata (inodes, bitmaps, group descs, directory blocks) is written via
 * ext4_write_block (write-through to the metadata cache).  File data bypasses
 * that cache and uses raw sector I/O, with the pagecache invalidated after a
 * write so reads re-fetch fresh data.  All of this runs under ext4_io_lock.
 * Perf note: metadata writeback here is eager (not batched the way fat32
 * defers FAT-sector flushes); large-file write throughput tuning is P7.
 * =================================================================== */

static int ext4_write_sectors(const block_device_t *bdev, unsigned long lba,
                              unsigned long count, const void *buf)
{
    if (!bdev || !bdev->write)
        return ST_UNSUPPORTED;
    might_sleep();
    unsigned long off = 0;
    unsigned ss = bdev->sector_size ? bdev->sector_size : 512;
    while (count > 0) {
        unsigned long chunk = (count > EXT4_MAX_SECTORS_PER_READ)
                            ? EXT4_MAX_SECTORS_PER_READ : count;
        int st = bdev->write((block_device_t *)bdev, lba, chunk,
                             (const uint8_t *)buf + off);
        if (st != ST_OK)
            return st;
        lba += chunk; off += chunk * ss; count -= chunk;
    }
    return ST_OK;
}

/* Write a metadata block straight to its final location, keeping any cached
 * copy coherent.  Bypasses the journal — used for checkpointing and outside
 * of transactions. */
static int ext4_write_block_direct(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
    if (pbn == 0)
        return ST_INVALID;
    if (fs->read_only)            /* error latch / read-only mount: refuse writes */
        return ST_ROFS;
    unsigned long lba = fs->part_lba_offset + pbn * fs->sectors_per_block;
    int st = ext4_write_sectors(fs->bdev, lba, fs->sectors_per_block, buf);
    if (st != ST_OK)
        return st;
    for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
        if (s_mbc[i].pbn == pbn && s_mbc[i].data) {
            mm_memcpy(s_mbc[i].data, buf, fs->block_size);
            break;
        }
    return ST_OK;
}

/* Write a metadata block.  Inside an active journaled transaction this only
 * buffers the block (it is journaled + checkpointed atomically at commit);
 * otherwise it goes straight to its final location. */
static int ext4_write_block(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
    if (pbn == 0)
        return ST_INVALID;
    if (s_txn.active && fs && fs->j_enabled) {
        ext4_txn_capture(fs, pbn, buf);
        return ST_OK;
    }
    return ext4_write_block_direct(fs, pbn, buf);
}

static int ext4_write_super(ext4_fs_t *fs)
{
    if (fs->has_metadata_csum)
        fs->sb_copy.s_checksum = ext4_sb_csum(&fs->sb_copy);
    unsigned ss = fs->bdev->sector_size ? fs->bdev->sector_size : 512;
    unsigned long lba = fs->part_lba_offset + EXT4_SUPERBLOCK_OFFSET / ss;
    unsigned sects = (sizeof(ext4_super_block) + ss - 1) / ss;
    return ext4_write_sectors(fs->bdev, lba, sects, &fs->sb_copy);
}

/* Central metadata-corruption handler — the ext4_error() analog.  Records the
 * error in the superblock (clears the "clean" bit, sets the error bit, bumps the
 * count) and persists it, then applies the errors= policy: remount read-only
 * (latch fs->read_only so all further writes are refused), panic, or continue.
 * The specific failed op still returns -EIO to its caller; this governs the
 * filesystem-wide reaction.  Idempotent: re-entry after the fs is already
 * errored re-records but does not re-apply the (one-shot) policy.  ext4_write_super
 * is intentionally NOT gated by the read-only latch so this can persist the mark. */
static void ext4_fs_error(ext4_fs_t *fs, const char *what, unsigned long ino)
{
    if (!fs) return;
    int first = !(fs->sb_copy.s_state & EXT4_ERROR_FS);
    if (ino) kprintf("ext4: ERROR: %s (inode %lu)\n", what, ino);
    else     kprintf("ext4: ERROR: %s\n", what);

    fs->sb_copy.s_state &= ~(uint16_t)EXT4_VALID_FS;
    fs->sb_copy.s_state |=  (uint16_t)EXT4_ERROR_FS;
    fs->sb_copy.s_error_count++;
    ext4_write_super(fs);                       /* best-effort; ungated by latch */
    if (fs->bdev && fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);

    if (!first) return;                         /* policy already applied once   */
    switch (fs->errors_behavior) {
        case EXT4_ERRORS_PANIC:
            kprintf("ext4: errors=panic -> halting\n");
            for (;;) __asm__ volatile("cli; hlt");
        case EXT4_ERRORS_CONTINUE:
            kprintf("ext4: errors=continue -> filesystem left writable\n");
            break;
        case EXT4_ERRORS_RO:
        default:
            fs->read_only = 1;                  /* latch: refuse further writes  */
            kprintf("ext4: remounting filesystem read-only\n");
            break;
    }
}

static int ext4_write_gd(ext4_fs_t *fs, unsigned int group);

/* Flush deferred group-descriptor + superblock free-count updates.  These are
 * advisory (e2fsck can recompute them), so instead of writing them on every
 * allocation we batch them once per top-level write operation. */
static void ext4_flush_meta(ext4_fs_t *fs)
{
    if (!fs->meta_dirty) return;
    for (unsigned g = 0; g < fs->groups_count; g++)
        ext4_write_gd(fs, g);
    ext4_write_super(fs);
    fs->meta_dirty = 0;
}

/* Persist the in-memory group descriptor for `group` back to its GDT block. */
static int ext4_write_gd(ext4_fs_t *fs, unsigned int group)
{
    unsigned long gdt_start = fs->first_data_block + 1;
    unsigned long per_block = fs->block_size / fs->desc_size;
    unsigned long blk = gdt_start + group / per_block;
    unsigned long off = (group % per_block) * fs->desc_size;
    uint8_t *buf = (uint8_t *)kalloc(fs->block_size);
    if (!buf)
        return ST_NOMEM;
    int st = ext4_read_block(fs, blk, buf);
    if (st != ST_OK) { kfree(buf); return st; }
    /* bitmap csums (in the descriptor) are already current; stamp bg_checksum
     * over the final descriptor bytes. */
    if (fs->has_metadata_csum)
        fs->gdt[group].bg_checksum = ext4_gd_csum(fs, group, &fs->gdt[group]);
    unsigned copy = fs->desc_size < sizeof(ext4_group_desc)
                  ? fs->desc_size : sizeof(ext4_group_desc);
    mm_memcpy(buf + off, &fs->gdt[group], copy);
    st = ext4_write_block(fs, blk, buf);
    kfree(buf);
    return st;
}

static inline int  ext4_bm_test(const uint8_t *bm, unsigned long b) { return (bm[b >> 3] >> (b & 7)) & 1; }
static inline void ext4_bm_set (uint8_t *bm, unsigned long b)       { bm[b >> 3] |= (uint8_t)(1u << (b & 7)); }
static inline void ext4_bm_clear(uint8_t *bm, unsigned long b)      { bm[b >> 3] &= (uint8_t)~(1u << (b & 7)); }

static unsigned long ext4_gd_block_bitmap(ext4_fs_t *fs, unsigned g)
{
    unsigned long lo = fs->gdt[g].bg_block_bitmap_lo;
    unsigned long hi = (fs->desc_size >= 64) ? fs->gdt[g].bg_block_bitmap_hi : 0;
    return lo | (hi << 32);
}
static unsigned long ext4_gd_inode_bitmap(ext4_fs_t *fs, unsigned g)
{
    unsigned long lo = fs->gdt[g].bg_inode_bitmap_lo;
    unsigned long hi = (fs->desc_size >= 64) ? fs->gdt[g].bg_inode_bitmap_hi : 0;
    return lo | (hi << 32);
}

/* Persist a full inode struct back to the inode table (RMW the table block). */
static int ext4_write_inode_struct(ext4_fs_t *fs, unsigned long ino,
                                   const ext4_inode *in)
{
    if (ino == 0 || ino > fs->inodes_count)
        return ST_INVALID;
    unsigned int group = (ino - 1) / fs->inodes_per_group;
    unsigned int index = (ino - 1) % fs->inodes_per_group;
    unsigned long itbl = ext4_inode_table_block(fs, &fs->gdt[group]);
    unsigned long byte = (unsigned long)index * fs->inode_size;
    unsigned long blk  = itbl + byte / fs->block_size;
    unsigned      off  = byte % fs->block_size;
    uint8_t *buf = (uint8_t *)kalloc(fs->block_size);
    if (!buf)
        return ST_NOMEM;
    int st = ext4_read_block(fs, blk, buf);
    if (st != ST_OK) { kfree(buf); return st; }
    unsigned copy = fs->inode_size < sizeof(ext4_inode) ? fs->inode_size : sizeof(ext4_inode);
    mm_memcpy(buf + off, in, copy);
    ext4_inode_csum_set(fs, ino, buf + off);   /* over the full on-disk inode */
    st = ext4_write_block(fs, blk, buf);
    kfree(buf);
    if (ino == s_ic_ino) s_ic_ino = 0;   /* parsed-inode cache now stale */
    return st;
}

/* bg_itable_unused: count of never-used inodes at the tail of the group's inode
 * table.  e2fsck honours it on metadata_csum / uninit_bg images (it skips that
 * tail during scanning), so allocating an inode past the tracked region must
 * shrink the count or e2fsck flags the freshly used inode as "in the unused
 * range" (and disagrees with the inode bitmap). */
static unsigned ext4_itable_unused(const ext4_fs_t *fs, unsigned g)
{
    unsigned v = fs->gdt[g].bg_itable_unused_lo;
    if (fs->desc_size >= 64)
        v |= (unsigned)fs->gdt[g].bg_itable_unused_hi << 16;
    return v;
}
static void ext4_itable_unused_set(ext4_fs_t *fs, unsigned g, unsigned v)
{
    fs->gdt[g].bg_itable_unused_lo = (uint16_t)(v & 0xFFFF);
    if (fs->desc_size >= 64)
        fs->gdt[g].bg_itable_unused_hi = (uint16_t)(v >> 16);
}

/* Allocate one free inode (preferring the parent's group). Returns 0 on fail. */
static unsigned long ext4_alloc_inode(ext4_fs_t *fs, unsigned long parent_ino, int is_dir)
{
    unsigned pgroup = parent_ino ? (unsigned)((parent_ino - 1) / fs->inodes_per_group) : 0;
    uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
    if (!bm) return 0;
    for (unsigned gi = 0; gi < fs->groups_count; gi++) {
        unsigned g = (pgroup + gi) % fs->groups_count;
        if (fs->gdt[g].bg_free_inodes_count_lo == 0) continue;
        unsigned long bblk = ext4_gd_inode_bitmap(fs, g);
        if (ext4_read_block(fs, bblk, bm) != ST_OK) continue;
        if (!ext4_inode_bitmap_csum_ok(fs, g, bm)) {
            ext4_fs_error(fs, "inode bitmap checksum mismatch", 0);
            kfree(bm); return 0;
        }
        for (unsigned i = 0; i < fs->inodes_per_group; i++) {
            if (ext4_bm_test(bm, i)) continue;
            unsigned long ino = (unsigned long)g * fs->inodes_per_group + i + 1;
            if (ino < fs->first_ino && ino != EXT4_ROOT_INO) continue;
            ext4_bm_set(bm, i);
            ext4_inode_bitmap_csum_set(fs, g, bm);
            /* shrink bg_itable_unused if we used an inode past the tracked front
             * region (mirrors the reference's ext4_new_inode). */
            if (fs->has_metadata_csum) {
                unsigned off1 = i + 1;   /* 1-based offset within the group */
                unsigned used_front = fs->inodes_per_group - ext4_itable_unused(fs, g);
                if (off1 > used_front)
                    ext4_itable_unused_set(fs, g, fs->inodes_per_group - off1);
            }
            if (ext4_write_block(fs, bblk, bm) != ST_OK) { kfree(bm); return 0; }
            fs->gdt[g].bg_free_inodes_count_lo--;
            if (is_dir) fs->gdt[g].bg_used_dirs_count_lo++;
            fs->sb_copy.s_free_inodes_count--;
            fs->meta_dirty = 1;
            kfree(bm);
            return ino;
        }
    }
    kfree(bm);
    return 0;
}

static void ext4_free_inode(ext4_fs_t *fs, unsigned long ino, int is_dir)
{
    if (ino == 0 || ino > fs->inodes_count) return;
    unsigned g = (ino - 1) / fs->inodes_per_group;
    unsigned i = (ino - 1) % fs->inodes_per_group;
    uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
    if (!bm) return;
    unsigned long bblk = ext4_gd_inode_bitmap(fs, g);
    if (ext4_read_block(fs, bblk, bm) == ST_OK) {
        if (!ext4_inode_bitmap_csum_ok(fs, g, bm)) {
            ext4_fs_error(fs, "inode bitmap checksum mismatch", 0);
            kfree(bm); return;
        }
        if (ext4_bm_test(bm, i)) {
            ext4_bm_clear(bm, i);
            ext4_inode_bitmap_csum_set(fs, g, bm);
            ext4_write_block(fs, bblk, bm);
            fs->gdt[g].bg_free_inodes_count_lo++;
            if (is_dir && fs->gdt[g].bg_used_dirs_count_lo) fs->gdt[g].bg_used_dirs_count_lo--;
            fs->sb_copy.s_free_inodes_count++;
            fs->meta_dirty = 1;
        }
    }
    kfree(bm);
}

static void ext4_free_block(ext4_fs_t *fs, unsigned long pbn)
{
    if (pbn < fs->first_data_block) return;
    /* If this block still has an outstanding journal copy (e.g. an rmdir freeing
     * a directory block written earlier this epoch), force a checkpoint after
     * the op so the stale copy can never be replayed over the block's next use.
     * This is why no jbd2 revoke records are needed (see the s_ckpt comment). */
    if (ext4_blk_journalled(pbn))
        s_force_ckpt = 1;
    unsigned g = (unsigned)((pbn - fs->first_data_block) / fs->blocks_per_group);
    if (g >= fs->groups_count) return;
    unsigned long i = (pbn - fs->first_data_block) % fs->blocks_per_group;
    uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
    if (!bm) return;
    unsigned long bblk = ext4_gd_block_bitmap(fs, g);
    if (ext4_read_block(fs, bblk, bm) == ST_OK) {
        if (!ext4_block_bitmap_csum_ok(fs, g, bm)) {
            ext4_fs_error(fs, "block bitmap checksum mismatch", 0);
            kfree(bm); return;
        }
        if (ext4_bm_test(bm, i)) {
            ext4_bm_clear(bm, i);
            ext4_block_bitmap_csum_set(fs, g, bm);
            ext4_write_block(fs, bblk, bm);
            fs->gdt[g].bg_free_blocks_count_lo++;
            fs->sb_copy.s_free_blocks_count_lo++;
            fs->meta_dirty = 1;
        }
    }
    ext4_mbc_drop(pbn);   /* avoid stale cache if this block is reallocated */
    kfree(bm);
}

/* Append logical block `lidx` -> physical `pbn` to an inline (depth-0) extent
 * tree.  Phase 2 supports the inline root only (up to 4 extents); growing the
 * tree depth is a later phase. */
static int ext4_extent_append(ext4_inode *in, unsigned long lidx, unsigned long pbn)
{
    ext4_extent_header *eh = (ext4_extent_header *)in->i_block;
    if (!(in->i_flags & EXT4_INODE_EXTENTS_FL) || eh->eh_magic != EXT4_EXT_MAGIC) {
        eh->eh_magic = EXT4_EXT_MAGIC; eh->eh_entries = 0; eh->eh_max = 4;
        eh->eh_depth = 0; eh->eh_generation = 0;
        in->i_flags |= EXT4_INODE_EXTENTS_FL;
    }
    if (eh->eh_depth != 0)
        return ST_UNSUPPORTED;   /* deep-tree writes: later phase */
    ext4_extent *ex = (ext4_extent *)(eh + 1);
    if (eh->eh_entries > 0) {
        ext4_extent *last = &ex[eh->eh_entries - 1];
        unsigned len = last->ee_len; if (len > 32768) len -= 32768;
        unsigned long lstart = (unsigned long)last->ee_start_lo
                             | ((unsigned long)last->ee_start_hi << 32);
        if (last->ee_block + len == lidx && lstart + len == pbn && len < 32768) {
            last->ee_len = (uint16_t)(len + 1);
            return ST_OK;
        }
    }
    if (eh->eh_entries >= eh->eh_max)
        return ST_UNSUPPORTED;   /* inline root full */
    ext4_extent *ne = &ex[eh->eh_entries];
    ne->ee_block = (uint32_t)lidx; ne->ee_len = 1;
    ne->ee_start_hi = (uint16_t)(pbn >> 32); ne->ee_start_lo = (uint32_t)pbn;
    eh->eh_entries++;
    return ST_OK;
}

/* Allocate `count` data blocks for inode `in`, appending them as extents
 * starting at logical index `start_lidx`.  Returns the number allocated
 * (may be < count if the group/extent-root fills up). */
static unsigned ext4_alloc_blocks_for_file(ext4_fs_t *fs, ext4_inode *in,
        unsigned long start_lidx, unsigned count)
{
    if (count == 0) return 0;
    uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
    if (!bm) return 0;
    unsigned allocated = 0;
    for (unsigned g = 0; g < fs->groups_count && allocated < count; g++) {
        if (fs->gdt[g].bg_free_blocks_count_lo == 0) continue;
        unsigned long bblk = ext4_gd_block_bitmap(fs, g);
        if (ext4_read_block(fs, bblk, bm) != ST_OK) continue;
        if (!ext4_block_bitmap_csum_ok(fs, g, bm)) {
            ext4_fs_error(fs, "block bitmap checksum mismatch", 0);
            kfree(bm); return allocated;
        }
        unsigned long base = fs->first_data_block + (unsigned long)g * fs->blocks_per_group;
        unsigned group_alloc = 0;
        int append_fail = 0;
        for (unsigned i = 0; i < fs->blocks_per_group && allocated < count; i++) {
            if (ext4_bm_test(bm, i)) continue;
            unsigned long pbn = base + i;
            if (ext4_extent_append(in, start_lidx + allocated, pbn) != ST_OK) {
                append_fail = 1; break;
            }
            ext4_bm_set(bm, i);
            allocated++; group_alloc++;
        }
        if (group_alloc) {
            ext4_block_bitmap_csum_set(fs, g, bm);
            ext4_write_block(fs, bblk, bm);
            fs->gdt[g].bg_free_blocks_count_lo -= group_alloc;
            fs->sb_copy.s_free_blocks_count_lo -= group_alloc;
            fs->meta_dirty = 1;
        }
        if (append_fail) break;
    }
    /* i_blocks counts 512-byte sectors used by the inode's data. */
    in->i_blocks_lo += allocated * (fs->block_size / 512);
    kfree(bm);
    return allocated;
}

/* Free every data block of inode `in` from logical index `from` onward, and
 * trim the inline extent tree accordingly.  Used by truncate/unlink. */
static void ext4_free_blocks_from(ext4_fs_t *fs, ext4_inode *in, unsigned long from)
{
    if (!(in->i_flags & EXT4_INODE_EXTENTS_FL)) return;   /* P2: extents only */
    ext4_extent_header *eh = (ext4_extent_header *)in->i_block;
    if (eh->eh_magic != EXT4_EXT_MAGIC || eh->eh_depth != 0) return;
    ext4_extent *ex = (ext4_extent *)(eh + 1);
    unsigned n = eh->eh_entries;
    unsigned keep = 0;
    unsigned long freed = 0;
    for (unsigned e = 0; e < n; e++) {
        unsigned long b0 = ex[e].ee_block;
        unsigned len = ex[e].ee_len; if (len > 32768) len -= 32768;
        unsigned long start = (unsigned long)ex[e].ee_start_lo
                            | ((unsigned long)ex[e].ee_start_hi << 32);
        if (b0 + len <= from) { keep++; continue; }           /* fully kept   */
        unsigned long cut = (from > b0) ? (from - b0) : 0;     /* keep [0,cut) */
        for (unsigned long k = cut; k < len; k++)
            ext4_free_block(fs, start + k);
        freed += (len - cut);
        if (cut > 0) { ex[e].ee_len = (uint16_t)cut; keep++; } /* shrink extent */
    }
    eh->eh_entries = (uint16_t)keep;
    unsigned long fsub = freed * (fs->block_size / 512);
    in->i_blocks_lo = (in->i_blocks_lo >= fsub) ? (in->i_blocks_lo - fsub) : 0;
}

/* ---- directory entry insert / remove ---- */
static inline unsigned ext4_dirent_len(unsigned name_len) { return (8 + name_len + 3) & ~3u; }

static int ext4_dir_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
                        unsigned name_len, unsigned long child_ino, unsigned ftype)
{
    ext4_inode din;
    if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
        return ST_IO;
    unsigned long dsize = ext4_inode_size(&din);
    unsigned long nblocks = dsize / fs->block_size;
    unsigned need = ext4_dirent_len(name_len);
    /* With metadata_csum the last 12 bytes of every leaf hold the checksum tail
     * (a fake entry) and must never be allocated into. */
    unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
    unsigned scan_limit = fs->block_size - tail;

    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk) return ST_NOMEM;

    for (unsigned long b = 0; b < nblocks; b++) {
        unsigned long pbn = ext4_block_map(fs, dir_ino, b);
        if (pbn == 0) continue;
        if (ext4_read_block(fs, pbn, blk) != ST_OK) continue;
        if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
            kfree(blk);
            ext4_fs_error(fs, "directory leaf checksum mismatch", dir_ino);
            return ST_IO;
        }
        unsigned off = 0;
        while (off + 8 <= scan_limit) {
            ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
            unsigned rec = de->rec_len;
            if (rec < 8 || off + rec > scan_limit) break;
            unsigned used = (de->inode == 0) ? 0 : ext4_dirent_len(de->name_len);
            if (rec - used >= need) {
                ext4_dir_entry_2 *ne;
                if (de->inode == 0) {
                    ne = de;                       /* reuse empty slot          */
                    ne->rec_len = (uint16_t)rec;
                } else {
                    de->rec_len = (uint16_t)used;  /* shrink, split off the tail */
                    ne = (ext4_dir_entry_2 *)(blk + off + used);
                    ne->rec_len = (uint16_t)(rec - used);
                }
                ne->inode = (uint32_t)child_ino;
                ne->name_len = (uint8_t)name_len;
                ne->file_type = (uint8_t)ftype;
                mm_memcpy(ne->name, name, name_len);
                ext4_dir_csum_set(fs, dir_ino, din.i_generation, blk);
                int st = ext4_write_block(fs, pbn, blk);
                kfree(blk);
                return st;
            }
            off += rec;
        }
    }

    /* No room in existing blocks: append a fresh directory block. */
    unsigned alloc = ext4_alloc_blocks_for_file(fs, &din, nblocks, 1);
    if (alloc != 1) { kfree(blk); return ST_NOMEM; }
    unsigned long pbn = ext4_block_map(fs, dir_ino, nblocks);
    /* ext4_block_map reads via the parsed cache; ensure it reflects new extent */
    s_ic_ino = 0;
    if (pbn == 0) {
        /* extent just appended to din (in-memory); compute directly */
        ext4_extent_header *eh = (ext4_extent_header *)din.i_block;
        ext4_extent *ex = (ext4_extent *)(eh + 1);
        ext4_extent *last = &ex[eh->eh_entries - 1];
        unsigned len = last->ee_len; if (len > 32768) len -= 32768;
        pbn = ((unsigned long)last->ee_start_lo | ((unsigned long)last->ee_start_hi << 32))
            + (nblocks - last->ee_block);
        (void)len;
    }
    mm_memset(blk, 0, fs->block_size);
    ext4_dir_entry_2 *ne = (ext4_dir_entry_2 *)blk;
    ne->inode = (uint32_t)child_ino;
    ne->rec_len = (uint16_t)(fs->block_size - tail);
    ne->name_len = (uint8_t)name_len;
    ne->file_type = (uint8_t)ftype;
    mm_memcpy(ne->name, name, name_len);
    ext4_dir_csum_set(fs, dir_ino, din.i_generation, blk);
    int st = ext4_write_block(fs, pbn, blk);
    kfree(blk);
    if (st != ST_OK) return st;
    /* grow directory size + persist the inode (with the new extent). */
    din.i_size_lo = (uint32_t)(dsize + fs->block_size);
    ext4_write_inode_struct(fs, dir_ino, &din);
    return ST_OK;
}

/* Remove `name` from directory `dir_ino`.  Returns the removed child inode in
 * *out_child (and its file_type in *out_ft).  Coalesces the freed record into
 * the previous entry. */
static int ext4_dir_del(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
                        unsigned name_len, unsigned long *out_child, unsigned *out_ft)
{
    ext4_inode din;
    if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
        return ST_IO;
    unsigned long nblocks = ext4_inode_size(&din) / fs->block_size;
    unsigned scan_limit = fs->block_size - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk) return ST_NOMEM;

    for (unsigned long b = 0; b < nblocks; b++) {
        unsigned long pbn = ext4_block_map(fs, dir_ino, b);
        if (pbn == 0) continue;
        if (ext4_read_block(fs, pbn, blk) != ST_OK) continue;
        if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
            kfree(blk);
            ext4_fs_error(fs, "directory leaf checksum mismatch", dir_ino);
            return ST_IO;
        }
        unsigned off = 0, prev = (unsigned)-1;
        while (off + 8 <= scan_limit) {
            ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
            unsigned rec = de->rec_len;
            if (rec < 8 || off + rec > scan_limit) break;
            if (de->inode != 0 && de->name_len == name_len &&
                ext4_memcmp(de->name, name, name_len) == 0) {
                if (out_child) *out_child = de->inode;
                if (out_ft) *out_ft = de->file_type;
                if (prev != (unsigned)-1) {
                    ext4_dir_entry_2 *pd = (ext4_dir_entry_2 *)(blk + prev);
                    pd->rec_len = (uint16_t)(pd->rec_len + rec);
                } else {
                    de->inode = 0;   /* first entry in block: just blank it */
                }
                ext4_dir_csum_set(fs, dir_ino, din.i_generation, blk);
                int st = ext4_write_block(fs, pbn, blk);
                kfree(blk);
                return st;
            }
            prev = off;
            off += rec;
        }
    }
    kfree(blk);
    return ST_NOT_FOUND;
}

/* Split a path into parent directory inode + final component name. */
static int ext4_resolve_parent(ext4_fs_t *fs, const char *path,
                               unsigned long *parent_ino, char *name_out, unsigned cap)
{
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    const char *base = (last >= 0) ? path + last + 1 : path;
    unsigned bl = 0; while (base[bl]) bl++;
    if (bl == 0 || bl >= cap) return ST_INVALID;
    for (unsigned i = 0; i <= bl; i++) name_out[i] = base[i];

    if (last <= 0) {   /* parent is root ('/foo') or cwd ('foo') */
        *parent_ino = (last == 0) ? EXT4_ROOT_INO : g_ext4_cwd_ino;
        return ST_OK;
    }
    char pdir[256];
    if (last >= (int)sizeof(pdir)) return ST_INVALID;
    for (int i = 0; i < last; i++) pdir[i] = path[i];
    pdir[last] = '\0';
    return ext4_resolve(fs, pdir, parent_ino);
}

/* Decide a new inode's owner/group/mode the way the reference does
 * (inode_init_owner), so behaviour matches exactly:
 *   - the creating process owns the inode (its fs-uid);
 *   - if the parent directory is set-group-ID, the new inode takes the parent's
 *     group, AND a new directory also inherits the set-group-ID bit; a new
 *     non-directory that is group-executable + set-group-ID, created by an
 *     unprivileged process that is not in the parent's group, has its
 *     set-group-ID bit dropped (no privilege escalation via group);
 *   - otherwise the group is the creator's fs-gid.
 * With no current task (early/kernel-time creation) inherit the parent's ids so
 * the boot-time, all-root tree is built exactly as before. */
static void ext4_init_owner(unsigned puid, unsigned pgid, unsigned pmode, int is_dir,
                            unsigned *uid, unsigned *gid, unsigned *mode)
{
    task_t *cur = sched_current();
    if (!cur) { *uid = puid; *gid = pgid; return; }
    *uid = (unsigned)cur->cred.fsuid;
    if (pmode & S_ISGID) {
        *gid = pgid;
        if (is_dir) {
            *mode |= S_ISGID;                       /* dirs always inherit it   */
        } else if (cur->cred.fsuid != 0 &&
                   (*mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP) &&
                   !cred_in_group(&cur->cred, pgid)) {
            *mode &= ~(unsigned)S_ISGID;            /* not a member: strip sgid */
        }
    } else {
        *gid = (unsigned)cur->cred.fsgid;
    }
}

/* Initialise + write a brand-new regular-file inode (owner/group/mode decided by
 * ext4_init_owner — the creating process, per the reference). */
static int ext4_create_inode(ext4_fs_t *fs, unsigned long ino, unsigned mode,
                             unsigned uid, unsigned gid)
{
    if (ext4_is_ro()) return ST_ROFS;
    ext4_inode in;
    mm_memset(&in, 0, sizeof(in));
    in.i_mode = (uint16_t)mode;
    in.i_uid = (uint16_t)uid;
    in.i_gid = (uint16_t)gid;
    in.i_links_count = 1;
    in.i_flags = EXT4_INODE_EXTENTS_FL;
    uint64_t now = timer_get_epoch();
    in.i_atime = in.i_ctime = in.i_mtime = (uint32_t)now;
    ext4_extent_header *eh = (ext4_extent_header *)in.i_block;
    eh->eh_magic = EXT4_EXT_MAGIC; eh->eh_max = 4;
    if (fs->inode_size > 128) in.i_extra_isize = 32;
    return ext4_write_inode_struct(fs, ino, &in);
}

static long ext4_write_impl(vfs_file_t *f, const void *buf, long bytes)
{
    if (!f || !buf) return ST_INVALID;
    ext4_file_t *ef = (ext4_file_t *)f->fs_private;
    if (!ef || !ef->fs) return ST_INVALID;
    if (ef->is_dir) return -EISDIR;
    if (bytes < 0) return ST_INVALID;
    if (bytes == 0) return 0;
    ext4_fs_t *fs = ef->fs;
    if (fs->read_only) return ST_ROFS;   /* error latch / read-only mount */

    unsigned long end = ef->pos + (unsigned long)bytes;
    ext4_inode in;
    if (ext4_read_inode_loc(fs, ef->ino, &in, 0, 0) != ST_OK) return ST_IO;

    unsigned long cur_size = ext4_inode_size(&in);
    unsigned long have = (cur_size + fs->block_size - 1) / fs->block_size;
    unsigned long need = (end + fs->block_size - 1) / fs->block_size;
    if (need > have) {
        unsigned got = ext4_alloc_blocks_for_file(fs, &in, have,
                                                  (unsigned)(need - have));
        if (got == 0) {
            ext4_flush_meta(fs);
            return -ENOSPC;
        }
        have += got;
        unsigned long max_end = have * fs->block_size;
        if (end > max_end) { end = max_end; bytes = (long)(end - ef->pos); }
    }

    /* Persist the inode ONCE: new extents (if any) + final size + mtime, so
     * the data loop's block_map sees the extents and we avoid a second write. */
    unsigned long newsize = (end > cur_size) ? end : cur_size;
    in.i_size_lo = (uint32_t)newsize;
    if ((in.i_mode & S_IFMT) == S_IFREG) in.i_size_high = (uint32_t)(newsize >> 32);
    {
        uint64_t now = timer_get_epoch();
        in.i_mtime = in.i_ctime = (uint32_t)now;
    }
    s_ic_ino = 0;
    ext4_write_inode_struct(fs, ef->ino, &in);

    /* Write the data, block by block (RMW on partial edges). */
    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk) return ST_NOMEM;
    unsigned long pos = ef->pos, remaining = (unsigned long)bytes, written = 0;
    const uint8_t *src = (const uint8_t *)buf;
    int io_err = 0;
    smap_disable();
    while (remaining) {
        unsigned long lidx = pos / fs->block_size;
        unsigned boff = pos % fs->block_size;
        unsigned chunk = fs->block_size - boff;
        if (chunk > remaining) chunk = (unsigned)remaining;
        unsigned long pbn = ext4_block_map(fs, ef->ino, lidx);
        if (pbn == 0) { io_err = 1; break; }
        if (boff != 0 || chunk != fs->block_size) {
            /* Partial block: read-modify-write.  Read the CURRENT on-disk data
             * with raw sector I/O — data blocks must NOT go through the
             * metadata block cache (it would serve stale content, since data
             * writes below use raw sectors and don't update that cache). */
            if (ext4_read_sectors(fs->bdev,
                    fs->part_lba_offset + pbn * fs->sectors_per_block,
                    fs->sectors_per_block, blk) != ST_OK)
                mm_memset(blk, 0, fs->block_size);
        } else {
            mm_memset(blk, 0, fs->block_size);
        }
        mm_memcpy(blk + boff, src + written, chunk);
        if (ext4_write_sectors(fs->bdev,
                fs->part_lba_offset + pbn * fs->sectors_per_block,
                fs->sectors_per_block, blk) != ST_OK) { io_err = 1; break; }
        pos += chunk; written += chunk; remaining -= chunk;
    }
    smap_enable();
    kfree(blk);

    if (written == 0 && io_err) return ST_IO;

    ef->pos += written;
    if (ef->pos > ef->size) ef->size = ef->pos;
    if (ef->inode) icache_update_size((ic_inode_t *)ef->inode, ef->size);
    /* Drop cached pages so subsequent reads see the freshly written data. */
    pagecache_invalidate_file(EXT4_BID_ENC(ef->ino, 0));
    icache_chain_invalidate(EXT4_BID_ENC(ef->ino, 0));
    ext4_flush_meta(fs);   /* batch the deferred GDT/superblock writeback */
    return (long)written;
}

static int ext4_truncate_impl(vfs_file_t *f, unsigned long size)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!f) return ST_INVALID;
    ext4_file_t *ef = (ext4_file_t *)f->fs_private;
    if (!ef || !ef->fs) return ST_INVALID;
    if (ef->is_dir) return -EISDIR;
    ext4_fs_t *fs = ef->fs;

    ext4_inode in;
    if (ext4_read_inode_loc(fs, ef->ino, &in, 0, 0) != ST_OK) return ST_IO;
    unsigned long cur = ext4_inode_size(&in);

    if (size < cur) {
        unsigned long from = (size + fs->block_size - 1) / fs->block_size;
        ext4_free_blocks_from(fs, &in, from);
    }
    in.i_size_lo = (uint32_t)size;
    if ((in.i_mode & S_IFMT) == S_IFREG) in.i_size_high = (uint32_t)(size >> 32);
    uint64_t now = timer_get_epoch();
    in.i_mtime = in.i_ctime = (uint32_t)now;
    s_ic_ino = 0;
    int st = ext4_write_inode_struct(fs, ef->ino, &in);
    ef->size = size;
    if (ef->pos > size) ef->pos = size;
    if (ef->inode) icache_update_size((ic_inode_t *)ef->inode, size);
    pagecache_invalidate_file(EXT4_BID_ENC(ef->ino, 0));
    icache_chain_invalidate(EXT4_BID_ENC(ef->ino, 0));
    ext4_flush_meta(fs);
    return st == ST_OK ? ST_OK : ST_IO;
}

static int ext4_unlink_impl(const char *path)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!path || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    unsigned long parent = 0;
    char name[256];
    if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;
    unsigned nl = 0; while (name[nl]) nl++;

    unsigned long child = 0; unsigned ft = 0;
    /* Look up first so we can reject directories and recover the inode. */
    if (ext4_dir_lookup(fs, parent, name, nl, &child, &ft) != ST_OK)
        return ST_NOT_FOUND;
    ext4_inode cin;
    if (ext4_read_inode_loc(fs, child, &cin, 0, 0) != ST_OK) return ST_IO;
    if ((cin.i_mode & S_IFMT) == S_IFDIR) return -EISDIR;

    if (ext4_dir_del(fs, parent, name, nl, 0, 0) != ST_OK)
        return ST_NOT_FOUND;

    if (cin.i_links_count > 0) cin.i_links_count--;
    if (cin.i_links_count == 0) {
        ext4_free_blocks_from(fs, &cin, 0);
        cin.i_dtime = (uint32_t)timer_get_epoch();
        cin.i_size_lo = 0; cin.i_size_high = 0;
        ext4_write_inode_struct(fs, child, &cin);
        ext4_free_inode(fs, child, 0);
        icache_remove(EXT4_BID_ENC(child, 0));
        pagecache_invalidate_file(EXT4_BID_ENC(child, 0));
    } else {
        cin.i_ctime = (uint32_t)timer_get_epoch();
        ext4_write_inode_struct(fs, child, &cin);
    }
    s_ic_ino = 0;
    ext4_flush_meta(fs);
    return ST_OK;
}

/* Fill a fresh directory block with "." (self) and ".." (parent) entries. */
static void ext4_init_dir_block(ext4_fs_t *fs, uint8_t *blk, unsigned long self_ino,
                                unsigned long parent_ino, uint32_t gen)
{
    unsigned bs = fs->block_size;
    unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
    mm_memset(blk, 0, bs);
    ext4_dir_entry_2 *dot = (ext4_dir_entry_2 *)blk;
    dot->inode = (uint32_t)self_ino; dot->rec_len = 12;
    dot->name_len = 1; dot->file_type = EXT4_FT_DIR; dot->name[0] = '.';
    ext4_dir_entry_2 *dd = (ext4_dir_entry_2 *)(blk + 12);
    dd->inode = (uint32_t)parent_ino; dd->rec_len = (uint16_t)(bs - 12 - tail);
    dd->name_len = 2; dd->file_type = EXT4_FT_DIR; dd->name[0] = '.'; dd->name[1] = '.';
    ext4_dir_csum_set(fs, self_ino, gen, blk);   /* lay out + checksum the tail */
}

/* Return 1 if the directory contains only "." and "..". */
static int ext4_dir_is_empty(ext4_fs_t *fs, unsigned long dir_ino)
{
    ext4_inode din;
    if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK) return 0;
    unsigned long nblocks = ext4_inode_size(&din) / fs->block_size;
    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk) return 0;
    int empty = 1;
    for (unsigned long b = 0; b < nblocks && empty; b++) {
        unsigned long pbn = ext4_block_map(fs, dir_ino, b);
        if (pbn == 0) continue;
        if (ext4_read_block(fs, pbn, blk) != ST_OK) continue;
        if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
            kfree(blk);
            ext4_fs_error(fs, "directory leaf checksum mismatch", dir_ino);
            return 0;   /* corrupt -> treat as "not empty": refuse the rmdir */
        }
        unsigned off = 0;
        while (off + 8 <= fs->block_size) {
            ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
            unsigned rec = de->rec_len;
            if (rec < 8 || off + rec > fs->block_size) break;
            if (de->inode != 0 &&
                !((de->name_len == 1 && de->name[0] == '.') ||
                  (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'))) {
                empty = 0; break;
            }
            off += rec;
        }
    }
    kfree(blk);
    return empty;
}

/* Point a directory's ".." entry at a new parent (used by cross-dir rename). */
static void ext4_dir_set_dotdot(ext4_fs_t *fs, unsigned long dir_ino,
                                unsigned long new_parent)
{
    const ext4_inode *di = ext4_get_inode_cached(fs, dir_ino);
    uint32_t gen = di ? di->i_generation : 0;
    unsigned long pbn = ext4_block_map(fs, dir_ino, 0);
    if (pbn == 0) return;
    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk) return;
    if (ext4_read_block(fs, pbn, blk) == ST_OK) {
        if (!ext4_dir_csum_ok(fs, dir_ino, gen, blk)) {
            ext4_fs_error(fs, "directory leaf checksum mismatch", dir_ino);
            kfree(blk); return;
        }
        unsigned off = 0;
        while (off + 8 <= fs->block_size) {
            ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
            unsigned rec = de->rec_len;
            if (rec < 8 || off + rec > fs->block_size) break;
            if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
                de->inode = (uint32_t)new_parent;
                ext4_dir_csum_set(fs, dir_ino, gen, blk);   /* re-stamp the tail */
                ext4_write_block(fs, pbn, blk);
                break;
            }
            off += rec;
        }
    }
    kfree(blk);
}

/* Adjust an inode's link count by `delta` and bump ctime. */
static void ext4_adjust_links(ext4_fs_t *fs, unsigned long ino, int delta)
{
    ext4_inode in;
    if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK) return;
    if (delta < 0 && in.i_links_count < (unsigned)(-delta)) in.i_links_count = 0;
    else in.i_links_count = (uint16_t)(in.i_links_count + delta);
    in.i_ctime = (uint32_t)timer_get_epoch();
    s_ic_ino = 0;
    ext4_write_inode_struct(fs, ino, &in);
}

static int ext4_mkdir_impl(const char *path, unsigned mode)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!path || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    unsigned long parent = 0; char name[256];
    if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;
    unsigned nl = 0; while (name[nl]) nl++;
    unsigned long existing;
    if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK)
        return ST_EXISTS;

    /* Owner/group of the new directory per the reference (the creating process;
     * a set-group-ID parent passes its group down and the new dir inherits the
     * set-group-ID bit via ext4_init_owner). */
    unsigned puid = 0, pgid = 0, pmode = 0;
    ext4_inode pin0;
    if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
        puid = pin0.i_uid; pgid = pin0.i_gid; pmode = pin0.i_mode;
    }
    unsigned nuid = puid, ngid = pgid;
    unsigned nmode = S_IFDIR | (mode ? (mode & 0777) : 0755);
    ext4_init_owner(puid, pgid, pmode, 1, &nuid, &ngid, &nmode);
    unsigned long nino = ext4_alloc_inode(fs, parent, 1);
    if (nino == 0) return ST_NOMEM;

    ext4_inode in; mm_memset(&in, 0, sizeof(in));
    in.i_mode = (uint16_t)nmode;
    in.i_uid = (uint16_t)nuid;
    in.i_gid = (uint16_t)ngid;
    in.i_links_count = 2;                 /* "." + the entry in parent       */
    in.i_flags = EXT4_INODE_EXTENTS_FL;
    uint32_t now = (uint32_t)timer_get_epoch();
    in.i_atime = in.i_ctime = in.i_mtime = now;
    ext4_extent_header *eh = (ext4_extent_header *)in.i_block;
    eh->eh_magic = EXT4_EXT_MAGIC; eh->eh_max = 4;
    if (fs->inode_size > 128) in.i_extra_isize = 32;

    if (ext4_alloc_blocks_for_file(fs, &in, 0, 1) != 1) {
        ext4_free_inode(fs, nino, 1); ext4_flush_meta(fs); return ST_NOMEM;
    }
    in.i_size_lo = fs->block_size;
    ext4_extent *ex = (ext4_extent *)(eh + 1);
    unsigned long pbn = (unsigned long)ex[0].ee_start_lo
                      | ((unsigned long)ex[0].ee_start_hi << 32);
    uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
    if (!blk) { ext4_free_blocks_from(fs, &in, 0); ext4_free_inode(fs, nino, 1);
                ext4_flush_meta(fs); return ST_NOMEM; }
    ext4_init_dir_block(fs, blk, nino, parent, in.i_generation);
    ext4_write_block(fs, pbn, blk);       /* dir block is metadata           */
    kfree(blk);
    s_ic_ino = 0;
    ext4_write_inode_struct(fs, nino, &in);

    if (ext4_dir_add(fs, parent, name, nl, nino, EXT4_FT_DIR) != ST_OK) {
        ext4_free_blocks_from(fs, &in, 0); ext4_free_inode(fs, nino, 1);
        ext4_flush_meta(fs); return ST_IO;
    }
    ext4_adjust_links(fs, parent, +1);    /* new dir's ".." references parent */
    s_ic_ino = 0;
    ext4_flush_meta(fs);
    return ST_OK;
}

static int ext4_rmdir_impl(const char *path)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!path || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    unsigned long parent = 0; char name[256];
    if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;
    unsigned nl = 0; while (name[nl]) nl++;
    if (nl == 1 && name[0] == '.') return ST_INVALID;

    unsigned long child = 0; unsigned ft = 0;
    if (ext4_dir_lookup(fs, parent, name, nl, &child, &ft) != ST_OK)
        return ST_NOT_FOUND;
    ext4_inode cin;
    if (ext4_read_inode_loc(fs, child, &cin, 0, 0) != ST_OK) return ST_IO;
    if ((cin.i_mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
    if (!ext4_dir_is_empty(fs, child)) return ST_NOTEMPTY;

    if (ext4_dir_del(fs, parent, name, nl, 0, 0) != ST_OK) return ST_NOT_FOUND;
    ext4_free_blocks_from(fs, &cin, 0);
    cin.i_links_count = 0; cin.i_size_lo = 0;
    cin.i_dtime = (uint32_t)timer_get_epoch();
    ext4_write_inode_struct(fs, child, &cin);
    ext4_free_inode(fs, child, 1);
    icache_remove(EXT4_BID_ENC(child, 0));
    pagecache_invalidate_file(EXT4_BID_ENC(child, 0));
    ext4_adjust_links(fs, parent, -1);    /* child's ".." is gone            */
    s_ic_ino = 0;
    ext4_flush_meta(fs);
    return ST_OK;
}

static int ext4_rename_impl(const char *oldp, const char *newp)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!oldp || !newp || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    unsigned long op = 0, np = 0; char oname[256], nname[256];
    if (ext4_resolve_parent(fs, oldp, &op, oname, sizeof(oname)) != ST_OK) return ST_NOT_FOUND;
    if (ext4_resolve_parent(fs, newp, &np, nname, sizeof(nname)) != ST_OK) return ST_NOT_FOUND;
    unsigned onl = 0; while (oname[onl]) onl++;
    unsigned nnl = 0; while (nname[nnl]) nnl++;

    unsigned long src = 0; unsigned sft = 0;
    if (ext4_dir_lookup(fs, op, oname, onl, &src, &sft) != ST_OK) return ST_NOT_FOUND;
    ext4_inode sin;
    if (ext4_read_inode_loc(fs, src, &sin, 0, 0) != ST_OK) return ST_IO;
    int src_is_dir = ((sin.i_mode & S_IFMT) == S_IFDIR);

    /* If the destination exists, remove it (only an existing *file* — leave
     * directory replacement to a later increment to keep link accounting
     * simple and correct). */
    unsigned long dst = 0; unsigned dft = 0;
    if (ext4_dir_lookup(fs, np, nname, nnl, &dst, &dft) == ST_OK) {
        if (dst == src) return ST_OK;     /* renaming to itself              */
        ext4_inode din;
        if (ext4_read_inode_loc(fs, dst, &din, 0, 0) != ST_OK) return ST_IO;
        if ((din.i_mode & S_IFMT) == S_IFDIR) return ST_EXISTS;
        ext4_dir_del(fs, np, nname, nnl, 0, 0);
        if (din.i_links_count > 0) din.i_links_count--;
        if (din.i_links_count == 0) {
            ext4_free_blocks_from(fs, &din, 0);
            din.i_size_lo = 0; din.i_dtime = (uint32_t)timer_get_epoch();
            ext4_write_inode_struct(fs, dst, &din);
            ext4_free_inode(fs, dst, 0);
            icache_remove(EXT4_BID_ENC(dst, 0));
            pagecache_invalidate_file(EXT4_BID_ENC(dst, 0));
        } else {
            ext4_write_inode_struct(fs, dst, &din);
        }
        s_ic_ino = 0;
    }

    if (ext4_dir_add(fs, np, nname, nnl, src, sft) != ST_OK) return ST_IO;
    ext4_dir_del(fs, op, oname, onl, 0, 0);

    if (src_is_dir && op != np) {
        ext4_dir_set_dotdot(fs, src, np);
        ext4_adjust_links(fs, op, -1);    /* src's old ".." left op          */
        ext4_adjust_links(fs, np, +1);    /* src's new ".." references np     */
    }
    s_ic_ino = 0;
    ext4_flush_meta(fs);
    return ST_OK;
}

/* UTIME sentinels — must match the values syscall.c passes (see fat32.h's
 * KRN_UTIME_NOW / KRN_UTIME_OMIT). */
#define EXT4_UTIME_NOW   1073741823L
#define EXT4_UTIME_OMIT  1073741822L

int ext4_utimensat(const char *path, int64_t mtime_sec, long mtime_nsec)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!path || !g_ext4_fs) return ST_INVALID;
    ext4_io_lock();
    ext4_fs_t *fs = g_ext4_fs;
    unsigned long ino = 0;
    int rr = ext4_resolve(fs, path, &ino);
    if (rr != ST_OK) { ext4_io_unlock(); return rr; } /* propagate ST_IO, not ENOENT */
    ext4_inode in;
    if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK) { ext4_io_unlock(); return ST_IO; }
    uint32_t now = (uint32_t)timer_get_epoch();
    if (mtime_nsec == EXT4_UTIME_OMIT) {
        /* leave mtime unchanged */
    } else if (mtime_nsec == EXT4_UTIME_NOW) {
        in.i_mtime = now;
    } else {
        in.i_mtime = (uint32_t)mtime_sec;
    }
    in.i_atime = now;
    in.i_ctime = now;
    s_ic_ino = 0;
    int rc = ext4_write_inode_struct(fs, ino, &in);
    ext4_io_unlock();
    return rc == ST_OK ? ST_OK : ST_IO;
}

int ext4_get_statfs(unsigned long *f_bsize, unsigned long *f_blocks,
                    unsigned long *f_bfree, unsigned long *f_files,
                    unsigned long *f_ffree, unsigned long *f_namelen,
                    unsigned long *f_type)
{
    if (!g_ext4_fs) return -1;
    ext4_fs_t *fs = g_ext4_fs;
    unsigned long bfree = (unsigned long)fs->sb_copy.s_free_blocks_count_lo;
    if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        bfree |= (unsigned long)fs->sb_copy.s_free_blocks_count_hi << 32;
    if (f_bsize)   *f_bsize   = fs->block_size;
    if (f_blocks)  *f_blocks  = fs->blocks_count;
    if (f_bfree)   *f_bfree   = bfree;
    if (f_files)   *f_files   = fs->inodes_count;
    if (f_ffree)   *f_ffree   = fs->sb_copy.s_free_inodes_count;
    if (f_namelen) *f_namelen = 255;
    if (f_type)    *f_type    = EXT4_SUPER_MAGIC;
    return 0;
}

/* ===================================================================
 * Phase 3: symlinks, hard links, chmod/chown, lstat.
 * =================================================================== */

int ext4_symlink(const char *target, const char *linkpath)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!target || !linkpath || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    ext4_io_lock();
    unsigned long parent = 0; char name[256];
    if (ext4_resolve_parent(fs, linkpath, &parent, name, sizeof(name)) != ST_OK) {
        ext4_io_unlock(); return ST_NOT_FOUND; }
    unsigned nl = 0; while (name[nl]) nl++;
    unsigned long existing;
    if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK) {
        ext4_io_unlock(); return ST_EXISTS; }
    unsigned tlen = 0; while (target[tlen]) tlen++;
    if (tlen >= fs->block_size) { ext4_io_unlock(); return ST_INVALID; }

    unsigned puid = 0, pgid = 0, pmode = 0; ext4_inode pin;
    if (ext4_read_inode_loc(fs, parent, &pin, 0, 0) == ST_OK) { puid = pin.i_uid; pgid = pin.i_gid; pmode = pin.i_mode; }
    unsigned nuid = puid, ngid = pgid, nmode = S_IFLNK | 0777;
    ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);

    unsigned long nino = ext4_alloc_inode(fs, parent, 0);
    if (nino == 0) { ext4_io_unlock(); return ST_NOMEM; }
    ext4_inode in; mm_memset(&in, 0, sizeof(in));
    in.i_mode = (uint16_t)nmode;
    in.i_uid = (uint16_t)nuid; in.i_gid = (uint16_t)ngid;
    in.i_links_count = 1;
    in.i_size_lo = tlen;
    uint32_t now = (uint32_t)timer_get_epoch();
    in.i_atime = in.i_ctime = in.i_mtime = now;
    if (fs->inode_size > 128) in.i_extra_isize = 32;

    if (tlen < 60) {                         /* fast symlink — inline target  */
        mm_memcpy(in.i_block, target, tlen);
        s_ic_ino = 0;
        ext4_write_inode_struct(fs, nino, &in);
    } else {                                 /* slow symlink — one data block  */
        in.i_flags = EXT4_INODE_EXTENTS_FL;
        ext4_extent_header *eh = (ext4_extent_header *)in.i_block;
        eh->eh_magic = EXT4_EXT_MAGIC; eh->eh_max = 4;
        if (ext4_alloc_blocks_for_file(fs, &in, 0, 1) != 1) {
            ext4_free_inode(fs, nino, 0); ext4_flush_meta(fs); ext4_io_unlock(); return ST_NOMEM; }
        ext4_extent *ex = (ext4_extent *)(eh + 1);
        unsigned long pbn = (unsigned long)ex[0].ee_start_lo | ((unsigned long)ex[0].ee_start_hi << 32);
        uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
        if (!blk) { ext4_free_blocks_from(fs, &in, 0); ext4_free_inode(fs, nino, 0);
                    ext4_flush_meta(fs); ext4_io_unlock(); return ST_NOMEM; }
        mm_memset(blk, 0, fs->block_size);
        mm_memcpy(blk, target, tlen);
        ext4_write_sectors(fs->bdev, fs->part_lba_offset + pbn * fs->sectors_per_block,
                           fs->sectors_per_block, blk);
        kfree(blk);
        s_ic_ino = 0;
        ext4_write_inode_struct(fs, nino, &in);
    }
    if (ext4_dir_add(fs, parent, name, nl, nino, EXT4_FT_SYMLINK) != ST_OK) {
        ext4_free_inode(fs, nino, 0); ext4_flush_meta(fs); ext4_io_unlock(); return ST_IO; }
    s_ic_ino = 0;
    ext4_flush_meta(fs);
    ext4_io_unlock();
    return ST_OK;
}

int ext4_readlink(const char *path, char *buf, unsigned long bufsiz)
{
    if (!path || !buf || !g_ext4_fs || bufsiz == 0) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    ext4_io_lock();
    unsigned long ino;
    if (ext4_resolve_ex(fs, g_ext4_cwd_ino, path, 0, &ino, 0) != ST_OK) {
        ext4_io_unlock(); return ST_NOT_FOUND; }
    const ext4_inode *in = ext4_get_inode_cached(fs, ino);
    if (!in) { ext4_io_unlock(); return ST_IO; }
    if ((in->i_mode & S_IFMT) != S_IFLNK) { ext4_io_unlock(); return ST_INVALID; }
    char tmp[256];
    int tl = ext4_read_symlink_target(fs, ino, in, tmp, sizeof(tmp));
    ext4_io_unlock();
    if (tl < 0) return ST_IO;
    unsigned long n = (unsigned long)tl;
    if (n > bufsiz) n = bufsiz;              /* readlink truncates, no NUL     */
    mm_memcpy(buf, tmp, n);
    return (int)n;
}

int ext4_link(const char *oldpath, const char *newpath)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!oldpath || !newpath || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    ext4_io_lock();
    unsigned long src;
    if (ext4_resolve_ex(fs, g_ext4_cwd_ino, oldpath, 1, &src, 0) != ST_OK) {
        ext4_io_unlock(); return ST_NOT_FOUND; }
    ext4_inode sin;
    if (ext4_read_inode_loc(fs, src, &sin, 0, 0) != ST_OK) { ext4_io_unlock(); return ST_IO; }
    if ((sin.i_mode & S_IFMT) == S_IFDIR) { ext4_io_unlock(); return ST_INVALID; }
    unsigned long parent; char name[256];
    if (ext4_resolve_parent(fs, newpath, &parent, name, sizeof(name)) != ST_OK) {
        ext4_io_unlock(); return ST_NOT_FOUND; }
    unsigned nl = 0; while (name[nl]) nl++;
    unsigned long existing;
    if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK) {
        ext4_io_unlock(); return ST_EXISTS; }
    unsigned ft = EXT4_FT_REG_FILE;
    if ((sin.i_mode & S_IFMT) == S_IFLNK) ft = EXT4_FT_SYMLINK;
    if (ext4_dir_add(fs, parent, name, nl, src, ft) != ST_OK) { ext4_io_unlock(); return ST_IO; }
    sin.i_links_count++;
    sin.i_ctime = (uint32_t)timer_get_epoch();
    s_ic_ino = 0;
    ext4_write_inode_struct(fs, src, &sin);
    ext4_flush_meta(fs);
    ext4_io_unlock();
    return ST_OK;
}

static int ext4_set_mode_locked(ext4_fs_t *fs, unsigned long ino, unsigned mode)
{
    ext4_inode in;
    if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK) return ST_IO;
    in.i_mode = (uint16_t)((in.i_mode & S_IFMT) | (mode & 07777));
    in.i_ctime = (uint32_t)timer_get_epoch();
    s_ic_ino = 0;
    /* A mode change can re-add a set-id bit: invalidate the per-inode strip
     * hint (S_NOSEC analog) for every handle, so the next non-root modify
     * re-evaluates.  Peek the cache without taking a ref. */
    ic_inode_t *ic = icache_lookup(EXT4_BID_ENC(ino, 0));
    if (ic) ic->flags &= ~(uint32_t)IC_SETID_CLEAN;
    return ext4_write_inode_struct(fs, ino, &in) == ST_OK ? ST_OK : ST_IO;
}

static int ext4_set_owner_locked(ext4_fs_t *fs, unsigned long ino, int uid, int gid)
{
    ext4_inode in;
    if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK) return ST_IO;
    if (uid >= 0) in.i_uid = (uint16_t)uid;
    if (gid >= 0) in.i_gid = (uint16_t)gid;
    in.i_ctime = (uint32_t)timer_get_epoch();
    s_ic_ino = 0;
    return ext4_write_inode_struct(fs, ino, &in) == ST_OK ? ST_OK : ST_IO;
}

int ext4_chmod(const char *path, unsigned mode)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!path || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    ext4_io_lock();
    unsigned long ino;
    int r = ext4_resolve(fs, path, &ino);
    if (r == ST_OK) r = ext4_set_mode_locked(fs, ino, mode);
    ext4_io_unlock();
    return r;
}

int ext4_chown(const char *path, int uid, int gid)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!path || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    ext4_io_lock();
    unsigned long ino;
    int r = ext4_resolve(fs, path, &ino);
    if (r == ST_OK) r = ext4_set_owner_locked(fs, ino, uid, gid);
    ext4_io_unlock();
    return r;
}

/* fd-based fchmod/fchown: returns the ext4 inode behind a vfs_file_t (0 if the
 * file is not an ext4 file), then chmod/chown by inode. */
unsigned long ext4_file_ino(struct vfs_file *f)
{
    if (!f || f->ops != &ext4_vfs_ops || !f->fs_private) return 0;
    return ((ext4_file_t *)f->fs_private)->ino;
}

int ext4_fchmod_ino(unsigned long ino, unsigned mode)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!ino || !g_ext4_fs) return ST_INVALID;
    ext4_io_lock();
    int r = ext4_set_mode_locked(g_ext4_fs, ino, mode);
    ext4_io_unlock();
    return r;
}

int ext4_fchown_ino(unsigned long ino, int uid, int gid)
{
    if (ext4_is_ro()) return ST_ROFS;
    if (!ino || !g_ext4_fs) return ST_INVALID;
    ext4_io_lock();
    int r = ext4_set_owner_locked(g_ext4_fs, ino, uid, gid);
    ext4_io_unlock();
    return r;
}

int ext4_lstat(const char *path, struct kstat *st)
{
    if (!path || !st || !g_ext4_fs) return ST_INVALID;
    ext4_fs_t *fs = g_ext4_fs;
    ext4_io_lock();
    unsigned long ino;
    int r = ext4_resolve_ex(fs, g_ext4_cwd_ino, path, 0, &ino, 0);  /* no follow final */
    if (r == ST_OK) r = ext4_stat_fill(fs, ino, st);
    ext4_io_unlock();
    return r;
}

static int ext4_fsync(vfs_file_t *f)
{
    ext4_io_lock();
    ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
    if (ef && !ef->is_dir) {
        pagecache_flush_file(EXT4_BID_ENC(ef->ino, 0));
        if (ef->inode) icache_flush((ic_inode_t *)ef->inode);
    }
    if (g_ext4_fs) ext4_flush_meta(g_ext4_fs);
    /* Commit this op's captured metadata now, then mark the journal clean: an
     * explicit sync is a consistency point, so a reboot after it should not
     * replay.  (ext4_txn_flush sets s_txn inactive, so the unlock below does
     * not re-commit.) */
    ext4_txn_flush(g_ext4_fs);
    if (g_ext4_fs && g_ext4_fs->bdev && g_ext4_fs->bdev->sync)
        g_ext4_fs->bdev->sync((block_device_t *)g_ext4_fs->bdev);
    ext4_journal_clean(g_ext4_fs);
    ext4_io_unlock();
    return 0;
}

/* Whole-filesystem sync (the sync(2) op, not tied to a file): flush deferred
 * metadata, commit any in-flight transaction, push the device, then mark the
 * journal clean so a reboot right after sync() does not replay.  Mirrors
 * ext4_fsync minus the per-file pagecache/icache flush. */
static int ext4_sync_op(void)
{
    if (!g_ext4_fs) return 0;
    ext4_io_lock();
    ext4_flush_meta(g_ext4_fs);
    ext4_txn_flush(g_ext4_fs);
    if (g_ext4_fs->bdev && g_ext4_fs->bdev->sync)
        g_ext4_fs->bdev->sync((block_device_t *)g_ext4_fs->bdev);
    ext4_journal_clean(g_ext4_fs);
    ext4_io_unlock();
    return 0;
}

/* ---- Locked wrappers (serialise via the reentrant sleeping mutex) ---- */
static int  ext4_open(const char *path, int flags, vfs_file_t **out)
{ ext4_io_lock(); int r = ext4_open_impl(path, flags, out); ext4_io_unlock(); return r; }
static int  ext4_stat_vfs(const char *path, struct kstat *st)
{ ext4_io_lock(); int r = ext4_stat_vfs_impl(path, st); ext4_io_unlock(); return r; }
static long ext4_read(vfs_file_t *f, void *buf, long bytes)
{ ext4_io_lock(); long r = ext4_read_impl(f, buf, bytes); ext4_io_unlock(); return r; }
static long ext4_write(vfs_file_t *f, const void *buf, long bytes)
{ ext4_io_lock(); long r = ext4_write_impl(f, buf, bytes); ext4_io_unlock(); return r; }
static long ext4_seek(vfs_file_t *f, long offset, int whence)
{ ext4_io_lock(); long r = ext4_seek_impl(f, offset, whence); ext4_io_unlock(); return r; }
static long ext4_readdir(vfs_file_t *f, void *buf, long bytes)
{ ext4_io_lock(); long r = ext4_readdir_impl(f, buf, bytes); ext4_io_unlock(); return r; }
static int  ext4_truncate(vfs_file_t *f, unsigned long size)
{ ext4_io_lock(); int r = ext4_truncate_impl(f, size); ext4_io_unlock(); return r; }
static int  ext4_unlink(const char *path)
{ ext4_io_lock(); int r = ext4_unlink_impl(path); ext4_io_unlock(); return r; }
static int  ext4_rename(const char *o, const char *n)
{ ext4_io_lock(); int r = ext4_rename_impl(o, n); ext4_io_unlock(); return r; }
static int  ext4_mkdir(const char *path, unsigned int mode)
{ ext4_io_lock(); int r = ext4_mkdir_impl(path, mode); ext4_io_unlock(); return r; }
static int  ext4_rmdir(const char *path)
{ ext4_io_lock(); int r = ext4_rmdir_impl(path); ext4_io_unlock(); return r; }
static int  ext4_chdir(const char *path)
{ ext4_io_lock(); int r = ext4_chdir_impl(path); ext4_io_unlock(); return r; }
static int  ext4_close(vfs_file_t *f)
{ ext4_io_lock(); int r = ext4_close_impl(f); ext4_io_unlock(); return r; }

static int ext4_release_locks_for_task(uint64_t task_id)
{
    int released = ext4_io_release_if_owner(task_id);
    if (g_ext4_fs && g_ext4_fs->bdev && g_ext4_fs->bdev->release_locks_for_task) {
        released |= g_ext4_fs->bdev->release_locks_for_task(
            (block_device_t *)g_ext4_fs->bdev, task_id);
    }
    return released;
}

/* vfs_ops adapters: address fchmod/fchown by open handle and fill the generic
 * statfs struct, so the syscall layer never calls an ext4_* symbol directly. */
static int ext4_fchmod_op(vfs_file_t *f, unsigned int mode)
{
    unsigned long ino = ext4_file_ino(f);
    if (!ino) return ST_OK;                 /* not an ext4 handle: no-op */
    return ext4_fchmod_ino(ino, mode);
}
static int ext4_fchown_op(vfs_file_t *f, int uid, int gid)
{
    unsigned long ino = ext4_file_ino(f);
    if (!ino) return ST_OK;
    return ext4_fchown_ino(ino, uid, gid);
}
static int ext4_statfs_op(struct vfs_statfs *out)
{
    if (!out) return ST_INVALID;
    unsigned long bs, blk, bf, fi, ff, nl, ty;
    if (ext4_get_statfs(&bs, &blk, &bf, &fi, &ff, &nl, &ty) != 0) return ST_IO;
    out->f_type = ty; out->f_bsize = bs; out->f_frsize = bs;
    out->f_blocks = blk; out->f_bfree = bf; out->f_bavail = bf;
    out->f_files = fi; out->f_ffree = ff; out->f_fsid = 0; out->f_namelen = nl;
    return ST_OK;
}

/* Fill a kstat for an open ext4 handle (real mode/uid/gid) — backs the fd-based
 * permission checks (fchmod/fchown ownership). */
static int ext4_fstat_op(vfs_file_t *f, struct kstat *st)
{
    if (!f || !st || !g_ext4_fs) return ST_INVALID;
    ext4_file_t *ef = (ext4_file_t *)f->fs_private;
    if (!ef) return ST_INVALID;
    ext4_io_lock();
    int r = ext4_stat_fill(g_ext4_fs, ef->ino, st);
    ext4_io_unlock();
    return r;
}

/* Per-inode set-id-strip hint (S_NOSEC analog).  Lives on the shared icache
 * entry, so every handle to the inode sees the same state.  Cheap: just a flag
 * bit, no I/O or io_lock — a race only costs a redundant strip evaluation. */
static int ext4_setid_clean_op(vfs_file_t *f, int mark)
{
    ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
    ic_inode_t *ic = ef ? (ic_inode_t *)ef->inode : 0;
    if (!ic) {
        /* No shared inode entry to carry the hint.  A regular file only lands
         * here if icache_get() failed at open (OOM): report "not clean" so the
         * strip still runs every modify (it reads the mode directly) — safe,
         * just unamortised.  Directories/special files have no inode by design
         * and must never be stripped (e.g. a set-group-ID dir), so: clean. */
        if (mark) return 1;                  /* nowhere to record it */
        return (ef && !ef->is_dir) ? 0 : 1;
    }
    if (mark) { ic->flags |= (uint32_t)IC_SETID_CLEAN; return 1; }
    return (ic->flags & IC_SETID_CLEAN) ? 1 : 0;
}

static const vfs_ops_t ext4_vfs_ops = {
    ext4_open, ext4_stat_vfs, ext4_read, ext4_write, ext4_seek, ext4_readdir,
    ext4_truncate, ext4_unlink, ext4_rename, ext4_mkdir, ext4_rmdir, ext4_chdir,
    ext4_close, ext4_release_locks_for_task, ext4_fsync,
    /* UNIX-semantics ops */
    ext4_lstat, ext4_symlink, ext4_readlink, ext4_link,
    ext4_chmod, ext4_chown, ext4_fchmod_op, ext4_fchown_op,
    ext4_utimensat, ext4_statfs_op, ext4_fstat_op, ext4_setid_clean_op,
    ext4_sync_op,
};

/* ===================================================================
 * Mount / probe / register
 * =================================================================== */

/* True if an ext4 superblock magic is present at (part_lba + sb offset). */
static int ext4_probe_offset(const block_device_t *bdev, unsigned long part_lba)
{
    unsigned ss = bdev->sector_size ? bdev->sector_size : 512;
    unsigned long sb_sector = part_lba + EXT4_SUPERBLOCK_OFFSET / ss;
    unsigned sb_sectors = (sizeof(ext4_super_block) + ss - 1) / ss;
    if (sb_sectors < 1) sb_sectors = 1;
    uint8_t *raw = (uint8_t *)kalloc(sb_sectors * ss);
    if (!raw) return 0;
    int ok = 0;
    if (ext4_read_sectors(bdev, sb_sector, sb_sectors, raw) == ST_OK) {
        ext4_super_block *sb = (ext4_super_block *)raw;
        if (sb->s_magic == EXT4_SUPER_MAGIC) ok = 1;
    }
    kfree(raw);
    return ok;
}

/* Locate an ext4 filesystem on a device: try the whole device first (the
 * legacy superfloppy / two-disk layout), then walk the GPT partition table and
 * probe each partition's start.  Returns the partition's starting LBA (0 for a
 * whole-device fs) or (unsigned long)-1 when no ext4 superblock is found.  This
 * lets the kernel mount the ext4 root partition of a single GPT USB disk whose
 * first partition is the FAT ESP. */
static unsigned long ext4_locate_partition(const block_device_t *bdev)
{
    unsigned ss = bdev->sector_size ? bdev->sector_size : 512;
    if (ext4_probe_offset(bdev, 0)) {
        kprintf("ext4: locate -> whole-device (ext4 magic at LBA0+1024)\n");
        return 0;
    }

    uint8_t *hdr = (uint8_t *)kalloc(ss);
    if (!hdr) return (unsigned long)-1;
    unsigned long found = (unsigned long)-1;

    /* GPT header is at LBA 1, signature "EFI PART". */
    if (ext4_read_sectors(bdev, 1, 1, hdr) == ST_OK &&
        hdr[0]=='E' && hdr[1]=='F' && hdr[2]=='I' && hdr[3]==' ' &&
        hdr[4]=='P' && hdr[5]=='A' && hdr[6]=='R' && hdr[7]=='T') {
        uint64_t ent_lba; uint32_t num_ent, ent_sz;
        mm_memcpy(&ent_lba, hdr + 72, sizeof(ent_lba));   /* PartitionEntryLBA */
        mm_memcpy(&num_ent, hdr + 80, sizeof(num_ent));   /* NumberOfEntries   */
        mm_memcpy(&ent_sz,  hdr + 84, sizeof(ent_sz));    /* SizeOfEntry       */
        if (ent_sz >= 56 && ent_sz <= 1024 && num_ent && num_ent <= 256) {
            unsigned long bytes = (unsigned long)num_ent * ent_sz;
            unsigned long secs  = (bytes + ss - 1) / ss;
            if (secs > 64) secs = 64;                     /* bound the read    */
            uint8_t *arr = (uint8_t *)kalloc(secs * ss);
            if (arr) {
                if (ext4_read_sectors(bdev, ent_lba, secs, arr) == ST_OK) {
                    unsigned long max_ent = (secs * ss) / ent_sz;
                    if (max_ent > num_ent) max_ent = num_ent;
                    for (unsigned long i = 0; i < max_ent; i++) {
                        uint8_t *e = arr + i * ent_sz;
                        int zero = 1;
                        for (int b = 0; b < 16; b++) if (e[b]) { zero = 0; break; }
                        if (zero) continue;               /* unused entry      */
                        uint64_t start;
                        mm_memcpy(&start, e + 32, sizeof(start));  /* StartingLBA */
                        if (start && ext4_probe_offset(bdev, (unsigned long)start)) {
                            found = (unsigned long)start;
                            break;
                        }
                    }
                }
                kfree(arr);
            }
        }
    }
    kfree(hdr);
    return found;
}

/* ===================================================================
 * Journal replay (jbd2 recovery) — PJ, replay-on-mount half.
 *
 * Closes the integrity hole where mount accepted the RECOVER incompat flag but
 * ignored it.  When the filesystem was not cleanly unmounted, jbd2 left
 * committed-but-not-yet-checkpointed transactions in the journal (inode #8);
 * we replay them to their final on-disk locations before any write touches the
 * filesystem.  Three passes over the circular log (classic jbd2 algorithm):
 *   SCAN   - find end_txn: the sequence just past the last COMMIT block.
 *   REVOKE - collect (block -> highest seq revoked) so superseded copies are
 *            not replayed.
 *   REPLAY - copy every committed descriptor tag's block to its final location
 *            unless a revoke at >= its sequence supersedes it.
 *
 * The jbd2 journal is BIG-ENDIAN on disk (unlike ext4 itself).  On a csum v2/v3
 * journal ALL three log checksums are verified on replay — the descriptor-block
 * TAIL csum (ext4_jdesc_csum_ok), each per-block TAG csum (ext4_jblock_csum),
 * and the per-transaction COMMIT-block csum (ext4_jcommit_csum_ok) — so a torn
 * or bit-rotted transaction is never applied (a bad descriptor/commit stops
 * replay there; a bad single block is not written).  The WRITE side stamps the
 * matching csum v3 (see ext4_txn_flush), so journaled writes are now enabled on
 * csum-v3 journals.  Replay is otherwise gated on the jbd2 magic + a contiguous
 * run of sequence numbers, correct for cleanly-written journals.  ASYNC_COMMIT
 * and fast-commit are still not specially handled (deferred, P6).
 * =================================================================== */

static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

/* Circular-log advance: log blocks live in [s_first, s_maxlen). */
static unsigned long jlog_advance(unsigned long cur, unsigned long n,
                                  unsigned long first, unsigned long maxlen)
{
    unsigned long span = (maxlen > first) ? (maxlen - first) : 0;
    if (span == 0) return cur;
    return first + (((cur - first) + n) % span);
}

/* Read journal log block `lbno` (journal-file logical block) into buf. */
static int jlog_read(ext4_fs_t *fs, unsigned long lbno, void *buf)
{
    unsigned long pbn = ext4_block_map(fs, fs->journal_inum, lbno);
    if (pbn == 0)
        return ST_IO;   /* the journal file must be fully allocated */
    return ext4_read_block(fs, pbn, buf);
}

/* Write journal log block `lbno` (raw, bypassing the metadata cache and the
 * transaction capture — these go to the on-disk journal, not final FS blocks). */
static int jlog_write(ext4_fs_t *fs, unsigned long lbno, const void *buf)
{
    unsigned long pbn = ext4_block_map(fs, fs->journal_inum, lbno);
    if (pbn == 0)
        return ST_IO;
    return ext4_write_sectors(fs->bdev,
                              fs->part_lba_offset + pbn * fs->sectors_per_block,
                              fs->sectors_per_block, buf);
}

/* Revoke table: dynamic array of {block, highest-revoked-seq}. */
typedef struct { unsigned long block; uint32_t seq; } jrev_ent_t;
typedef struct { jrev_ent_t *v; unsigned n, cap; } jrev_tbl_t;

static void jrev_add(jrev_tbl_t *t, unsigned long block, uint32_t seq)
{
    for (unsigned i = 0; i < t->n; i++)
        if (t->v[i].block == block) {            /* keep the latest revoke   */
            if (seq > t->v[i].seq) t->v[i].seq = seq;
            return;
        }
    if (t->n == t->cap) {
        unsigned ncap = t->cap ? t->cap * 2 : 64;
        jrev_ent_t *nv = (jrev_ent_t *)kalloc(ncap * sizeof(jrev_ent_t));
        if (!nv) { WARN_ON_ONCE(1); return; }     /* drop: at worst over-replay */
        if (t->v) { mm_memcpy(nv, t->v, t->n * sizeof(jrev_ent_t)); kfree(t->v); }
        t->v = nv; t->cap = ncap;
    }
    t->v[t->n].block = block;
    t->v[t->n].seq   = seq;
    t->n++;
}

/* A block is superseded (skip replay) if revoked at a seq >= the txn seq. */
static int jrev_test(const jrev_tbl_t *t, unsigned long block, uint32_t seq)
{
    for (unsigned i = 0; i < t->n; i++)
        if (t->v[i].block == block && t->v[i].seq >= seq)
            return 1;
    return 0;
}

/* jbd2 csum v2/v3 seed = crc32c(~0, journal_uuid).  Folds with the BE
 * transaction sequence for the per-block TAG csum; used directly for the
 * descriptor-tail and commit-block csums.  (The journal SUPERBLOCK csum is
 * different: seed ~0 over the whole 1024-byte sb — see ext4_jsb_csum_set.) */
static inline uint32_t ext4_jcsum_seed(const journal_superblock_t *jsb)
{
    return ext4_crc32c(0xFFFFFFFFu, jsb->s_uuid, sizeof(jsb->s_uuid));
}

/* Per-block TAG checksum (csum v2/v3): crc32c(seed, BE(seq)) folded with the
 * journalled (escaped) block bytes.  v3 stores the full 32 bits in the tag,
 * v2 the low 16.  Computed over the exact bytes written to the log, so callers
 * must verify BEFORE un-escaping (and stamp AFTER escaping). */
static uint32_t ext4_jblock_csum(uint32_t seed, uint32_t seq,
                                 const void *blk, unsigned bs)
{
    uint32_t seq_be = __builtin_bswap32(seq);
    uint32_t c = ext4_crc32c(seed, &seq_be, sizeof(seq_be));
    return ext4_crc32c(c, blk, bs);
}

/* Descriptor/revoke TAIL checksum (csum v2/v3): the last 4 bytes hold
 * crc32c(seed, whole block) computed with that tail field zeroed. */
static int ext4_jdesc_csum_ok(uint32_t seed, uint8_t *blk, unsigned bs)
{
    uint32_t *ptail = (uint32_t *)(blk + bs - 4);
    uint32_t raw = *ptail;
    *ptail = 0;
    uint32_t calc = ext4_crc32c(seed, blk, bs);
    *ptail = raw;
    return be32(raw) == calc;
}

/* jbd2 csum v2/v3 commit-block checksum (big-endian on disk).  The commit block
 * is valid iff crc32c(seed, block) with h_chksum[0] zeroed equals the stored
 * value.  h_chksum[0] sits at offset 0x10 (12-byte journal_header +
 * h_chksum_type/size/pad = 4).  A torn or bit-rotted commit fails this, so on a
 * csum journal we stop replay there instead of applying an incomplete
 * transaction.  FAIL-SAFE: a false negative only stops replay earlier (fs stays
 * consistent, as if the crash happened a txn sooner). */
static int ext4_jcommit_csum_ok(uint32_t seed, uint8_t *blk, unsigned bs)
{
    uint32_t *pchk = (uint32_t *)(blk + JBD2_COMMIT_CSUM_OFF);
    uint32_t raw = *pchk;                       /* stored be32 csum             */
    *pchk = 0;
    uint32_t calc = ext4_crc32c(seed, blk, bs);
    *pchk = raw;                                /* restore the buffer           */
    return be32(raw) == calc;
}

/* Stamp the journal SUPERBLOCK checksum (csum v2/v3): s_checksum @0xFC =
 * crc32c(~0, first 1024 bytes with that field zeroed).  Note the ~0 seed (the
 * uuid is inside the summed region), unlike the tag/desc/commit csums.  No-op
 * unless `enabled`.  `jsbbuf` is a full-block buffer. */
static void ext4_jsb_csum_set(uint8_t *jsbbuf, int enabled)
{
    if (!enabled) return;
    uint32_t *pc = (uint32_t *)(jsbbuf + JBD2_SB_CSUM_OFF);
    *pc = 0;
    *pc = __builtin_bswap32(ext4_crc32c(0xFFFFFFFFu, jsbbuf, JBD2_SB_CSUM_LEN));
}

#define EXT4_JPASS_SCAN   0
#define EXT4_JPASS_REVOKE 1
#define EXT4_JPASS_REPLAY 2

/* One pass over the committed log.  SCAN sets *end_txn (sequence past the last
 * commit); REVOKE/REPLAY are bounded by it.  Returns ST_OK or an I/O error. */
static int ext4_journal_pass(ext4_fs_t *fs, const journal_superblock_t *jsb,
                             int pass, uint32_t *end_txn, jrev_tbl_t *revtbl,
                             unsigned long *out_replayed)
{
    unsigned long first  = be32(jsb->s_first);
    unsigned long maxlen = be32(jsb->s_maxlen);
    uint32_t jincompat   = be32(jsb->s_feature_incompat);
    int      is64    = (jincompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0;
    int      csum3   = (jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0;
    int      csum2   = (jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) != 0;
    int      csum_any = csum2 || csum3;           /* descriptor/commit csums    */
    unsigned tag_sz  = csum3 ? 16u : (is64 ? 12u : 8u);
    uint32_t jseed   = ext4_jcsum_seed(jsb);

    unsigned long next_log = be32(jsb->s_start);
    uint32_t      next_seq = be32(jsb->s_sequence);
    unsigned long replayed = 0;
    int rc = ST_OK;

    uint8_t *blk  = (uint8_t *)kalloc(fs->block_size);
    uint8_t *data = (uint8_t *)kalloc(fs->block_size);
    if (!blk || !data) { if (blk) kfree(blk); if (data) kfree(data); return ST_NOMEM; }

    for (;;) {
        if (pass != EXT4_JPASS_SCAN && next_seq >= *end_txn)
            break;                                 /* all committed txns done */
        if (jlog_read(fs, next_log, blk) != ST_OK) { rc = ST_IO; break; }
        const journal_header_t *h = (const journal_header_t *)blk;
        if (be32(h->h_magic) != JBD2_MAGIC_NUMBER) break;   /* end of log     */
        if (be32(h->h_sequence) != next_seq)       break;   /* stale: end     */

        uint32_t bt = be32(h->h_blocktype);
        if (bt == JBD2_DESCRIPTOR_BLOCK) {
            /* csum v2/v3: verify the descriptor TAIL csum before trusting any
             * tag.  A bad tail means a torn/corrupt descriptor — stop here, so
             * the tag walk never advances next_log off garbage. */
            if (csum_any && !ext4_jdesc_csum_ok(jseed, blk, fs->block_size)) {
                if (pass == EXT4_JPASS_SCAN)
                    kprintf("ext4: journal descriptor csum bad at seq %u; "
                            "stopping replay there\n", next_seq);
                break;
            }
            unsigned      off      = sizeof(journal_header_t);
            unsigned long data_idx = 0;
            while (off + tag_sz <= fs->block_size) {
                const uint8_t *tag = blk + off;
                uint32_t blocknr = be32(*(const uint32_t *)(tag + 0));
                uint32_t flags   = csum3 ? be32(*(const uint32_t *)(tag + JBD2_TAG3_FLAGS_OFF))
                                         : be16(*(const uint16_t *)(tag + 6));
                unsigned long target = blocknr;
                if (is64)                              /* high 32 at +8 (both)*/
                    target |= ((unsigned long)be32(*(const uint32_t *)(tag + JBD2_TAG3_BLKHI_OFF)) << 32);

                if (pass == EXT4_JPASS_REPLAY) {
                    /* The journalled copy is 1 + data_idx blocks past the
                     * descriptor in the log. */
                    unsigned long dl = jlog_advance(next_log, 1 + data_idx, first, maxlen);
                    if (jlog_read(fs, dl, data) != ST_OK) { rc = ST_IO; goto out; }
                    /* Per-block TAG csum (csum v2/v3) is over the journalled
                     * (escaped) bytes — verify BEFORE restoring the magic. */
                    int tagbad = 0;
                    if (csum_any) {
                        uint32_t want = ext4_jblock_csum(jseed, next_seq, data, fs->block_size);
                        int ok = csum3
                            ? (be32(*(const uint32_t *)(tag + JBD2_TAG3_CSUM_OFF)) == want)
                            : (be16(*(const uint16_t *)(tag + JBD2_TAG_V2_CSUM_OFF)) == (uint16_t)want);
                        if (!ok) {
                            kprintf("ext4: journal block tag csum bad (block %lu, "
                                    "seq %u); not applying that block\n", target, next_seq);
                            tagbad = 1;
                        }
                    }
                    if (flags & JBD2_FLAG_ESCAPE) {     /* restore escaped magic */
                        uint32_t m = __builtin_bswap32(JBD2_MAGIC_NUMBER);
                        mm_memcpy(data, &m, 4);
                    }
                    if (target == 0) {
                        WARN_ON_ONCE(1);               /* tag targets block 0 */
                    } else if (!tagbad && !jrev_test(revtbl, target, next_seq)) {
                        if (ext4_write_block(fs, target, data) != ST_OK) { rc = ST_IO; goto out; }
                        replayed++;
                    }
                }
                data_idx++;
                off += tag_sz;
                if (!(flags & JBD2_FLAG_SAME_UUID))
                    off += 16;                         /* a UUID follows tag  */
                if (flags & JBD2_FLAG_LAST_TAG)
                    break;
            }
            next_log = jlog_advance(next_log, 1 + data_idx, first, maxlen);
        } else if (bt == JBD2_COMMIT_BLOCK) {
            if (csum_any && !ext4_jcommit_csum_ok(jseed, blk, fs->block_size)) {
                /* Torn/corrupt commit on a csum journal: this transaction never
                 * completed, so it — and everything after — must NOT be replayed. */
                if (pass == EXT4_JPASS_SCAN)
                    kprintf("ext4: journal commit csum bad at seq %u; "
                            "stopping replay there\n", next_seq);
                break;
            }
            next_seq++;                                /* transaction boundary */
            if (pass == EXT4_JPASS_SCAN)
                *end_txn = next_seq;
            next_log = jlog_advance(next_log, 1, first, maxlen);
        } else if (bt == JBD2_REVOKE_BLOCK) {
            if (pass == EXT4_JPASS_REVOKE) {
                const jbd2_revoke_header_t *rh = (const jbd2_revoke_header_t *)blk;
                unsigned used = be32(rh->r_count);
                if (used > fs->block_size) used = fs->block_size;
                unsigned rsz = is64 ? 8u : 4u;
                unsigned p = sizeof(jbd2_revoke_header_t);
                while (p + rsz <= used) {
                    unsigned long rb = be32(*(const uint32_t *)(blk + p));
                    if (is64)
                        rb |= ((unsigned long)be32(*(const uint32_t *)(blk + p + 4)) << 32);
                    jrev_add(revtbl, rb, next_seq);
                    p += rsz;
                }
            }
            next_log = jlog_advance(next_log, 1, first, maxlen);
        } else {
            break;                                     /* unknown type: end   */
        }
    }
out:
    kfree(blk);
    kfree(data);
    if (out_replayed) *out_replayed = replayed;
    return rc;
}

/* Replay the journal of `fs`.  Safe to call on every mount that has a journal:
 * it reads the journal superblock and no-ops silently when the log is clean
 * (s_start == 0).  This makes the journal's own log-tail pointer the source of
 * truth for "needs recovery", independent of the ext4 RECOVER flag — the two
 * live in different sectors and a device write-back cache can persist one
 * without the other across a crash (observed: VMware kept the committed
 * journal but not the RECOVER bit, so a RECOVER-only gate missed the replay).
 * Sets *did_work = 1 iff blocks were actually replayed (caller must then drop
 * its metadata caches).  Returns ST_OK on success including the no-op cases. */
static int ext4_journal_recover(ext4_fs_t *fs, int *did_work)
{
    if (did_work) *did_work = 0;
    if (fs->journal_inum == 0)
        return ST_OK;
    uint8_t *jblk = (uint8_t *)kalloc(fs->block_size);
    if (!jblk) return ST_NOMEM;

    /* The journal superblock is journal log block 0. */
    unsigned long sb_pbn = ext4_block_map(fs, fs->journal_inum, 0);
    if (sb_pbn == 0 || ext4_read_block(fs, sb_pbn, jblk) != ST_OK) {
        kfree(jblk); return ST_IO;
    }
    journal_superblock_t *jsb = (journal_superblock_t *)jblk;
    if (be32(jsb->s_header.h_magic) != JBD2_MAGIC_NUMBER) {
        kprintf("ext4: bad journal superblock magic\n");
        kfree(jblk); return ST_INVALID;
    }
    uint32_t sbt = be32(jsb->s_header.h_blocktype);
    if (sbt != JBD2_SUPERBLOCK_V1 && sbt != JBD2_SUPERBLOCK_V2) {
        kprintf("ext4: unexpected journal sb blocktype %u\n", sbt);
        kfree(jblk); return ST_INVALID;
    }
    if (be32(jsb->s_blocksize) != fs->block_size) {
        kprintf("ext4: journal blocksize %u != fs %u (unsupported)\n",
                be32(jsb->s_blocksize), fs->block_size);
        kfree(jblk); return ST_UNSUPPORTED;
    }

    uint32_t jincompat = be32(jsb->s_feature_incompat);
    uint32_t known = JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT
                   | JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3
                   | JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT | JBD2_FEATURE_INCOMPAT_FAST_COMMIT;
    if (jincompat & ~known)
        kprintf("ext4: journal unknown incompat 0x%x; attempting replay anyway\n",
                jincompat & ~known);
    if (jincompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))
        kprintf("ext4: journal csum-v%d: descriptor/tag/commit csums verified on replay\n",
                (jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) ? 3 : 2);
    if (jincompat & (JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT | JBD2_FEATURE_INCOMPAT_FAST_COMMIT))
        kprintf("ext4: journal async/fast-commit not specially handled (tolerant replay)\n");

    /* Always report the journal state at mount so a "did it recover?" question
     * is never a guess.  s_start is the log tail: 0 == clean (nothing to
     * replay), non-zero == a committed-but-uncheckpointed transaction exists. */
    uint32_t jstart = be32(jsb->s_start);
    kprintf("ext4: journal state s_start=%u seq=%u recover_flag=%d\n",
            jstart, be32(jsb->s_sequence),
            (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) ? 1 : 0);
    if (jstart == 0) {
        kfree(jblk); return ST_OK;        /* clean log: nothing to replay     */
    }
    kprintf("ext4: replaying journal (s_start=%u)...\n", jstart);

    /* Pass 1 — scan to the end of the committed log. */
    uint32_t   end_txn = be32(jsb->s_sequence);
    jrev_tbl_t revtbl  = { 0, 0, 0 };
    int rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_SCAN, &end_txn, &revtbl, 0);
    if (rc != ST_OK) { kfree(jblk); return rc; }
    if (end_txn == be32(jsb->s_sequence)) {
        kprintf("ext4: journal has no committed transactions; nothing to replay\n");
        kfree(jblk); return ST_OK;
    }

    /* Pass 2 — collect revoke records (only if the journal uses them). */
    if (jincompat & JBD2_FEATURE_INCOMPAT_REVOKE) {
        rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_REVOKE, &end_txn, &revtbl, 0);
        if (rc != ST_OK) { if (revtbl.v) kfree(revtbl.v); kfree(jblk); return rc; }
    }

    /* Pass 3 — replay committed blocks to their final locations. */
    unsigned long replayed = 0;
    rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_REPLAY, &end_txn, &revtbl, &replayed);
    if (revtbl.v) kfree(revtbl.v);
    if (rc != ST_OK) { kfree(jblk); return rc; }

    /* Mark the journal empty: s_start = 0, s_sequence = end_txn.  On a csum
     * journal the superblock carries its own checksum (seed ~0 over 1024 bytes),
     * so restamp it after editing or Linux/e2fsck will reject the journal sb. */
    jsb->s_start    = __builtin_bswap32(0);
    jsb->s_sequence = __builtin_bswap32(end_txn);
    ext4_jsb_csum_set(jblk, (jincompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2
                                        | JBD2_FEATURE_INCOMPAT_CSUM_V3)) != 0);
    if (ext4_write_block(fs, sb_pbn, jblk) != ST_OK) { kfree(jblk); return ST_IO; }
    if (fs->bdev && fs->bdev->sync)
        fs->bdev->sync((block_device_t *)fs->bdev);   /* persist before RECOVER clears */

    kprintf("ext4: journal replay complete (%lu block(s) recovered, end_txn=%u)\n",
            replayed, end_txn);
    if (did_work && replayed > 0) *did_work = 1;
    kfree(jblk);
    return ST_OK;
}

/* Checkpoint the open epoch: write every pending (committed-but-not-yet-final)
 * metadata block to its final location, make it durable, then mark the journal
 * log empty (s_start=0).  TWO device syncs, amortised over the whole epoch.
 * Ordering: the final writes are made durable BEFORE s_start=0, so a crash mid-
 * checkpoint replays the epoch again (idempotent), and a crash after s_start=0
 * finds the final data already durable.  Leaves RECOVER set (only
 * ext4_journal_clean clears it) — s_start=0 alone makes recovery a no-op.
 * Called with s_txn inactive, so the writeback goes direct. */
static void ext4_checkpoint(ext4_fs_t *fs)
{
    if (!fs || !fs->j_enabled || !fs->j_sb_buf || !s_epoch_open)
        return;
    for (unsigned i = 0; i < s_ckpt.n; i++)         /* 1. pending -> final     */
        ext4_write_block_direct(fs, s_ckpt.blk[i], s_ckpt.data[i]);
    if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);

    journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
    jsb->s_start    = 0;                             /* 2. empty the log        */
    jsb->s_sequence = __builtin_bswap32(fs->j_sequence);
    ext4_jsb_csum_set(fs->j_sb_buf, fs->j_csum3);
    jlog_write(fs, 0, fs->j_sb_buf);
    if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);

    s_ckpt.n     = 0;                                /* 3. epoch closed         */
    s_epoch_open = 0;
    s_jhead      = fs->j_first;
}

/* ===================================================================
 * PJ/P7: journaled writes (ordered mode), deferred-checkpoint circular log.
 *
 * Called from ext4_io_unlock on the outermost release with the lock held.  Each
 * op's buffered metadata (s_txn) is journalled to the log head and made durable
 * with ONE device sync, then recorded in the in-memory epoch pending set
 * (s_ckpt) — it is NOT checkpointed to its final location yet.  Steady-state
 * cost is therefore one sync per op; the 2-sync checkpoint (ext4_checkpoint) is
 * amortised over a whole epoch and runs only when the log/pending set fills, an
 * op frees a still-journalled block (s_force_ckpt), or on fsync/sync.
 *
 * Crash safety: recovery reads s_start (the epoch start, == j_first, made
 * durable by the epoch's first commit sync) and replays every committed txn in
 * sequence order until the chain breaks (torn/absent commit, validated by the
 * csum-v3 commit + descriptor + tag csums).  Because checkpoint is deferred, the
 * committed metadata lives only in the log until then, so reads consult s_ckpt
 * (read-your-writes) — critically, the allocator reads bitmaps from there, so it
 * never double-allocates a block the epoch already used.  Monotonic sequences +
 * never letting an epoch wrap (we checkpoint before the head reaches j_maxlen)
 * mean a stale older epoch left in the log can never extend the new chain.
 * Data blocks are written in place during the op and flushed by the commit sync
 * ("ordered").  No jbd2 revoke records: the only journalled blocks we free are
 * directory blocks, and freeing any still-journalled block forces a checkpoint
 * (s_force_ckpt) that empties the log before the block can be reused.
 * =================================================================== */
static void ext4_txn_flush(ext4_fs_t *fs)
{
    if (!s_txn.active)
        return;
    s_txn.active = 0;                 /* capture off: writes below go direct */
    unsigned n = s_txn.n;
    s_txn.n = 0;
    if (n == 0)
        return;                       /* read-only op: nothing to commit     */

    if (!fs || !fs->j_enabled || !fs->j_sb_buf) {   /* journaling off: direct */
        for (unsigned i = 0; i < n; i++)
            ext4_write_block_direct(fs, s_txn.blk[i], s_txn.data[i]);
        return;
    }

    unsigned long first  = fs->j_first;
    unsigned long maxlen = fs->j_maxlen;
    uint32_t      seq    = fs->j_sequence;
    unsigned long span   = (maxlen > first) ? (maxlen - first) : 0;

    journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
    int      is64  = (be32(jsb->s_feature_incompat) & JBD2_FEATURE_INCOMPAT_64BIT) != 0;
    int      csum3 = fs->j_csum3;                  /* stamp jbd2 csum v3 if set  */
    unsigned tagsz = csum3 ? JBD2_TAG3_SIZE : (is64 ? 12u : 8u);

    /* All tags + the lone tag0 uuid + (csum v3) the descriptor tail must fit in
     * one descriptor block — we don't chain descriptors.  Plus the whole txn
     * (descriptor + n data + commit) must fit the log.  Either overflow → drain
     * the epoch, then fall back to direct in-place writes for this op. */
    unsigned desc_need = sizeof(journal_header_t) + n * tagsz + 16 + (csum3 ? 4u : 0u);
    if (n + 2 > span || desc_need > fs->block_size) {
        WARN_ON_ONCE(1);
        ext4_checkpoint(fs);
        for (unsigned i = 0; i < n; i++)
            ext4_write_block_direct(fs, s_txn.blk[i], s_txn.data[i]);
        if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);
        return;
    }

    /* Make room: checkpoint the current epoch first (resets s_jhead = j_first)
     * if this txn would run off the log end, exceed the per-epoch log-block bound
     * (keeps crash recovery fast), or exceed the pending-set memory cap. */
    if (s_jhead + (n + 2) > maxlen ||
        (s_jhead - first) + (n + 2) > EXT4_EPOCH_MAX_BLOCKS ||
        s_ckpt.n + n > EXT4_CKPT_MAX_BLOCKS)
        ext4_checkpoint(fs);

    /* 1. mark needs-recovery, durably, once per dirty period (amortised). */
    if (!(fs->sb_copy.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER)) {
        fs->sb_copy.s_feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
        fs->feature_incompat           |= EXT4_FEATURE_INCOMPAT_RECOVER;
        ext4_write_super(fs);
        if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);
    }

    unsigned long pos = s_jhead;     /* descriptor @pos, data @pos+1.., commit @pos+1+n */

    /* 2. On the epoch's FIRST txn, point the journal sb at the epoch start
     * (== j_first; the log is contiguous from there).  Written but NOT synced
     * here — folded into the step-3 commit sync.  Later txns leave s_start /
     * s_sequence unchanged (recovery follows the sequence chain forward). */
    if (!s_epoch_open) {
        s_epoch_seq = seq;
        jsb->s_start    = __builtin_bswap32((uint32_t)first);
        jsb->s_sequence = __builtin_bswap32(seq);
        ext4_jsb_csum_set(fs->j_sb_buf, csum3);
        jlog_write(fs, 0, fs->j_sb_buf);
    }

    /* 3. descriptor block + data blocks, then the commit block, at the head. */
    uint8_t *db  = (uint8_t *)kalloc(fs->block_size);
    uint8_t *cpy = (uint8_t *)kalloc(fs->block_size);
    if (!db || !cpy) {                /* OOM: drain epoch + direct for this op */
        if (db) kfree(db);
        if (cpy) kfree(cpy);
        ext4_checkpoint(fs);
        for (unsigned i = 0; i < n; i++)
            ext4_write_block_direct(fs, s_txn.blk[i], s_txn.data[i]);
        if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);
        return;
    }
    mm_memset(db, 0, fs->block_size);
    journal_header_t *dh = (journal_header_t *)db;
    dh->h_magic     = __builtin_bswap32(JBD2_MAGIC_NUMBER);
    dh->h_blocktype = __builtin_bswap32(JBD2_DESCRIPTOR_BLOCK);
    dh->h_sequence  = __builtin_bswap32(seq);
    unsigned off = sizeof(journal_header_t);
    for (unsigned i = 0; i < n; i++) {
        unsigned long tgt = s_txn.blk[i];
        uint32_t flags = 0;
        if (i > 0)     flags |= JBD2_FLAG_SAME_UUID;   /* uuid only after tag0 */
        if (i == n - 1) flags |= JBD2_FLAG_LAST_TAG;
        uint32_t fw;
        mm_memcpy(&fw, s_txn.data[i], 4);
        int escape = (fw == __builtin_bswap32(JBD2_MAGIC_NUMBER));
        if (escape) flags |= JBD2_FLAG_ESCAPE;

        /* The journalled (escaped) copy must exist before the tag csum, which is
         * computed over the exact bytes written to the log. */
        mm_memcpy(cpy, s_txn.data[i], fs->block_size);
        if (escape) { uint32_t z = 0; mm_memcpy(cpy, &z, 4); }

        uint8_t *tag = db + off;
        *(uint32_t *)(tag + 0) = __builtin_bswap32((uint32_t)(tgt & 0xFFFFFFFFUL));
        if (csum3) {                                   /* 16-byte csum-v3 tag  */
            *(uint32_t *)(tag + JBD2_TAG3_FLAGS_OFF) = __builtin_bswap32(flags);
            *(uint32_t *)(tag + JBD2_TAG3_BLKHI_OFF) =
                is64 ? __builtin_bswap32((uint32_t)(tgt >> 32)) : 0;
            uint32_t tc = ext4_jblock_csum(fs->j_csum_seed, seq, cpy, fs->block_size);
            *(uint32_t *)(tag + JBD2_TAG3_CSUM_OFF) = __builtin_bswap32(tc);
        } else {                                       /* classic 8/12-byte tag*/
            *(uint16_t *)(tag + 4) = 0;                /* t_checksum (no csum) */
            *(uint16_t *)(tag + 6) = __builtin_bswap16((uint16_t)flags);
            if (is64)
                *(uint32_t *)(tag + 8) = __builtin_bswap32((uint32_t)(tgt >> 32));
        }
        off += tagsz;
        if (!(flags & JBD2_FLAG_SAME_UUID)) {          /* journal uuid follows */
            mm_memcpy(db + off, fs->j_uuid, 16);
            off += 16;
        }
        jlog_write(fs, pos + 1 + i, cpy);
    }
    /* csum v3: the descriptor's TAIL csum covers the whole block (tags + their
     * per-block csums) with the tail field zeroed — stamp it after the loop. */
    if (csum3) {
        uint32_t *dt = (uint32_t *)(db + fs->block_size - 4);
        *dt = 0;
        *dt = __builtin_bswap32(ext4_crc32c(fs->j_csum_seed, db, fs->block_size));
    }
    jlog_write(fs, pos, db);                            /* the descriptor      */
    mm_memset(cpy, 0, fs->block_size);                 /* commit block        */
    journal_header_t *ch = (journal_header_t *)cpy;
    ch->h_magic     = __builtin_bswap32(JBD2_MAGIC_NUMBER);
    ch->h_blocktype = __builtin_bswap32(JBD2_COMMIT_BLOCK);
    ch->h_sequence  = __builtin_bswap32(seq);
    if (csum3) {                                       /* h_chksum_type/size=0 */
        uint32_t *cc = (uint32_t *)(cpy + JBD2_COMMIT_CSUM_OFF);
        *cc = 0;                                       /* h_chksum[0] zeroed   */
        *cc = __builtin_bswap32(ext4_crc32c(fs->j_csum_seed, cpy, fs->block_size));
    }
    jlog_write(fs, pos + 1 + n, cpy);
    if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);  /* commit durable */
    kfree(db);
    kfree(cpy);

    /* 4. record the committed txn in the epoch pending set (no checkpoint yet)
     * and advance the log head + sequence. */
    for (unsigned i = 0; i < n; i++)
        ext4_ckpt_merge(fs, s_txn.blk[i], s_txn.data[i]);
    s_jhead      = pos + n + 2;
    s_epoch_open = 1;
    fs->j_sequence = seq + 1;

    /* 5. If this op freed a block that still had a journal copy, checkpoint now
     * so that stale copy can never be replayed over the block's reuse. */
    if (s_force_ckpt) {
        ext4_checkpoint(fs);
        s_force_ckpt = 0;
    }
}

/* Clean the journal: checkpoint any open epoch to its final locations, mark the
 * log empty (s_start=0) and drop needs-recovery.  Called on explicit sync /
 * fsync / shutdown — NOT per commit.  After this the on-disk fs is fully
 * consistent and a reboot finds a clean fs that does not replay. */
static void ext4_journal_clean(ext4_fs_t *fs)
{
    if (!fs || !fs->j_enabled || !fs->j_sb_buf)
        return;
    ext4_checkpoint(fs);              /* flush pending epoch -> final, s_start=0 */
    if (!(fs->sb_copy.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER))
        return;                                   /* already clean */
    journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
    if (jsb->s_start != 0) {                       /* ensure log empty on disk   */
        jsb->s_start    = 0;
        jsb->s_sequence = __builtin_bswap32(fs->j_sequence);
        ext4_jsb_csum_set(fs->j_sb_buf, fs->j_csum3);
        jlog_write(fs, 0, fs->j_sb_buf);
        if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);
    }
    fs->sb_copy.s_feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
    fs->feature_incompat           &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
    ext4_write_super(fs);
    if (fs->bdev->sync) fs->bdev->sync((block_device_t *)fs->bdev);
}

int ext4_mount(const block_device_t *bdev, ext4_fs_t *out)
{
    if (!bdev || !out)
        return ST_INVALID;
    unsigned ss = bdev->sector_size ? bdev->sector_size : 512;

    /* Find the ext4 filesystem (whole-device or a GPT partition). */
    unsigned long part_lba = ext4_locate_partition(bdev);
    if (part_lba == (unsigned long)-1)
        return ST_NOT_FOUND;

    /* The ext4 superblock is at byte offset 1024 (sector 2 for 512B sectors),
     * relative to the partition start. */
    unsigned long sb_sector = part_lba + EXT4_SUPERBLOCK_OFFSET / ss;
    unsigned sb_sectors = (sizeof(ext4_super_block) + ss - 1) / ss;
    if (sb_sectors < 1) sb_sectors = 1;

    uint8_t *raw = (uint8_t *)kalloc(sb_sectors * ss);
    if (!raw)
        return ST_NOMEM;
    if (ext4_read_sectors(bdev, sb_sector, sb_sectors, raw) != ST_OK) {
        kfree(raw);
        return ST_IO;
    }
    ext4_super_block *sb = (ext4_super_block *)raw;
    if (sb->s_magic != EXT4_SUPER_MAGIC) {
        kfree(raw);
        return ST_NOT_FOUND;
    }
    mm_memset(out, 0, sizeof(*out));
    mm_memcpy(&out->sb_copy, sb, sizeof(out->sb_copy));
    out->bdev = bdev;
    out->part_lba_offset = part_lba;   /* 0 for whole-device, else GPT part start */
    out->block_size = 1024u << sb->s_log_block_size;
    out->sectors_per_block = out->block_size / ss;
    out->inode_size = (sb->s_rev_level == 0) ? EXT4_GOOD_OLD_INODE_SIZE
                                             : sb->s_inode_size;
    if (out->inode_size < 128) out->inode_size = 128;
    out->inodes_per_group = sb->s_inodes_per_group;
    out->blocks_per_group = sb->s_blocks_per_group;
    out->inodes_count = sb->s_inodes_count;
    out->blocks_count = (unsigned long)sb->s_blocks_count_lo
                      | ((unsigned long)sb->s_blocks_count_hi << 32);
    out->first_data_block = sb->s_first_data_block;
    out->feature_incompat = sb->s_feature_incompat;
    out->feature_ro_compat = sb->s_feature_ro_compat;
    out->feature_compat = sb->s_feature_compat;
    out->first_ino = (sb->s_rev_level == 0) ? EXT4_GOOD_OLD_FIRST_INO
                                            : sb->s_first_ino;
    out->journal_inum = sb->s_journal_inum;
    out->read_only = 0;   /* Phase 2: writes enabled */
    /* errors= policy for runtime corruption (ext4_fs_error).  Linux applies a
     * mount-time default of remount-ro on metadata corruption regardless of the
     * on-disk s_errors hint (mke2fs stamps s_errors=continue by default), since
     * leaving a corrupt filesystem writable risks compounding the damage.  We do
     * the same: default to the safe remount-ro, honouring only an explicit panic
     * request from the superblock.  A deliberate errors=continue would require a
     * mount option, which we don't parse yet. */
    out->errors_behavior =
        (sb->s_errors == EXT4_ERRORS_PANIC) ? EXT4_ERRORS_PANIC
                                            : EXT4_ERRORS_RO;

    if (out->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        out->desc_size = sb->s_desc_size ? sb->s_desc_size : 64;
    else
        out->desc_size = 32;

    if (out->blocks_per_group == 0) { kfree(raw); return ST_INVALID; }
    out->groups_count = (unsigned int)
        ((out->blocks_count - out->first_data_block + out->blocks_per_group - 1)
         / out->blocks_per_group);

    /* P6: metadata_csum seed + (verify-only) superblock checksum.  The seed is
     * s_checksum_seed when the CSUM_SEED feature is set, else crc32c(~0, uuid). */
    out->has_metadata_csum =
        (out->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
    if (out->feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
        out->csum_seed = sb->s_checksum_seed;
    else
        out->csum_seed = ext4_crc32c(0xFFFFFFFFu, sb->s_uuid, sizeof(sb->s_uuid));
    if (out->has_metadata_csum) {
        uint32_t got = ext4_sb_csum(sb), want = sb->s_checksum;
        if (got != want) {
            /* The superblock is corrupt — we can't trust its geometry for
             * writes.  Degrade to a read-only mount (don't refuse to boot / panic
             * the root fs) and mark it errored in-memory so e2fsck is run. */
            kprintf("ext4: ERROR superblock metadata_csum mismatch "
                    "(disk 0x%x computed 0x%x) -> mounting read-only\n", want, got);
            out->sb_copy.s_state &= ~(uint16_t)EXT4_VALID_FS;
            out->sb_copy.s_state |=  (uint16_t)EXT4_ERROR_FS;
            out->read_only = 1;
        } else
            kprintf("ext4: metadata_csum on; superblock csum verified\n");
    }

    kfree(raw);

    /* Reject incompat features we cannot safely read. */
    uint32_t known_incompat = EXT4_FEATURE_INCOMPAT_FILETYPE
                            | EXT4_FEATURE_INCOMPAT_RECOVER
                            | EXT4_FEATURE_INCOMPAT_EXTENTS
                            | EXT4_FEATURE_INCOMPAT_64BIT
                            | EXT4_FEATURE_INCOMPAT_FLEX_BG
                            | EXT4_FEATURE_INCOMPAT_META_BG
                            | EXT4_FEATURE_INCOMPAT_INLINE_DATA
                            | EXT4_FEATURE_INCOMPAT_CSUM_SEED;
    if (out->feature_incompat & ~known_incompat) {
        kprintf("ext4: unsupported incompat features 0x%x\n",
                out->feature_incompat & ~known_incompat);
        return ST_UNSUPPORTED;
    }

    s_ic_ino = 0;   /* reset the single-entry inode cache */
    ext4_mbc_invalidate();
    if (ext4_load_gdt(out) != ST_OK) {
        kprintf("ext4: failed to load group descriptor table\n");
        return ST_IO;
    }

    /* P6 (verify-only): confirm every group descriptor's metadata_csum, which
     * exercises the csum_seed derivation.  Warn with a count; never reject. */
    if (out->has_metadata_csum) {
        unsigned bad = 0;
        for (unsigned int g = 0; g < out->groups_count; g++)
            if (ext4_gd_csum(out, g, &out->gdt[g]) != out->gdt[g].bg_checksum)
                bad++;
        if (bad) {
            /* A corrupt group descriptor would mis-place bitmaps / inode tables —
             * degrade to read-only and mark errored rather than write through it. */
            kprintf("ext4: ERROR %u/%u group-desc metadata_csum mismatch "
                    "-> mounting read-only\n", bad, out->groups_count);
            out->sb_copy.s_state &= ~(uint16_t)EXT4_VALID_FS;
            out->sb_copy.s_state |=  (uint16_t)EXT4_ERROR_FS;
            out->read_only = 1;
        } else
            kprintf("ext4: all %u group-desc csums verified\n", out->groups_count);
    }

    /* Journal replay: apply any committed transactions left in the journal
     * BEFORE any write touches the filesystem.  Triggered by the journal's own
     * log-tail pointer (s_start != 0), NOT just the ext4 RECOVER flag — those
     * are separate sectors and a device write-back cache can persist the
     * journal commit without the RECOVER bit across a crash (seen on VMware),
     * so a RECOVER-only gate silently misses real recovery work. */
    if (out->journal_inum != 0) {
        int replayed = 0;
        int rr = ext4_journal_recover(out, &replayed);
        if (rr == ST_OK) {
            if (replayed) {
                /* Replay rewrote final metadata under us; drop caches that
                 * were populated before/by replay so later reads see the
                 * recovered state. */
                ext4_mbc_invalidate();
                s_ic_ino = 0;
                ext4_load_gdt(out);     /* GDT blocks may have been replayed   */
            }
            /* Clear RECOVER (if it was set) now the journal is clean again. */
            if (out->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
                out->feature_incompat            &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
                out->sb_copy.s_feature_incompat  &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
                ext4_write_super(out);
                if (out->bdev && out->bdev->sync)
                    out->bdev->sync((block_device_t *)out->bdev);
            }
        } else {
            /* Leave RECOVER set so the next mount retries (replay is
             * idempotent).  Warn loudly: this matches today's behaviour of
             * mounting a dirty journal, no worse, and recoverable on reboot. */
            kprintf("ext4: WARNING journal replay failed (%d); RECOVER left set\n", rr);
            WARN_ON_ONCE(1);
        }
    }

    /* Resync the superblock free counts to the (authoritative, journalled) group
     * descriptors.  The per-group counts are journalled and recovered exactly,
     * but the superblock totals are written eagerly/direct by ext4_flush_meta and
     * are NOT journalled, so after a crash the on-disk superblock total can drift
     * from the recovered bitmaps (e2fsck: "free blocks count wrong").  Summing the
     * GDT (which matches the bitmaps) makes them consistent again — self-healing
     * on every mount; a clean fs already matches so nothing is written. */
    {
        int is64 = (out->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
        uint64_t fb = 0; uint32_t fi = 0;
        for (unsigned g = 0; g < out->groups_count; g++) {
            uint32_t gb = out->gdt[g].bg_free_blocks_count_lo;
            uint32_t gi = out->gdt[g].bg_free_inodes_count_lo;
            if (is64) {
                gb |= (uint32_t)out->gdt[g].bg_free_blocks_count_hi << 16;
                gi |= (uint32_t)out->gdt[g].bg_free_inodes_count_hi << 16;
            }
            fb += gb; fi += gi;
        }
        uint32_t fb_lo = (uint32_t)fb, fb_hi = (uint32_t)(fb >> 32);
        if (out->sb_copy.s_free_blocks_count_lo != fb_lo ||
            out->sb_copy.s_free_blocks_count_hi != fb_hi ||
            out->sb_copy.s_free_inodes_count    != fi) {
            out->sb_copy.s_free_blocks_count_lo = fb_lo;
            out->sb_copy.s_free_blocks_count_hi = fb_hi;
            out->sb_copy.s_free_inodes_count    = fi;
            ext4_write_super(out);
            if (out->bdev && out->bdev->sync)
                out->bdev->sync((block_device_t *)out->bdev);
            kprintf("ext4: resynced superblock free counts (blocks=%lu inodes=%u)\n",
                    (unsigned long)fb, fi);
        }
    }

    /* PJ: set up journaled writes (ordered mode).  Disabled unless the journal
     * exists, matches our block size, and uses only features we can WRITE
     * correctly.  We stamp jbd2 csum v3, so a csum-v3 journal is supported; csum
     * v2, async-commit and fast-commit remain unsupported on the write side and
     * fall back to direct writes.  On a metadata_csum fs whose journal is still
     * plain (mke2fs ships it that way — the kernel upgrades it on first rw mount)
     * we upgrade it to csum-v3 here, matching Linux, so our own images get
     * csum-protected journaling.  Must run after recovery so j_sequence reflects
     * the post-recovery journal state (and the log is clean before any upgrade). */
    out->j_enabled = 0;
    out->j_sb_buf  = 0;
    out->j_csum3   = 0;
    if ((out->feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) && out->journal_inum != 0) {
        out->j_sb_buf = (uint8_t *)kalloc(out->block_size);
        unsigned long jpbn = out->j_sb_buf ? ext4_block_map(out, out->journal_inum, 0) : 0;
        if (out->j_sb_buf && jpbn &&
            ext4_read_block(out, jpbn, out->j_sb_buf) == ST_OK) {
            journal_superblock_t *jsb = (journal_superblock_t *)out->j_sb_buf;
            uint32_t jmag = be32(jsb->s_header.h_magic);
            uint32_t jbt  = be32(jsb->s_header.h_blocktype);
            uint32_t jinc = be32(jsb->s_feature_incompat);
            uint32_t jbad = JBD2_FEATURE_INCOMPAT_CSUM_V2
                          | JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT | JBD2_FEATURE_INCOMPAT_FAST_COMMIT;
            if (jmag == JBD2_MAGIC_NUMBER &&
                (jbt == JBD2_SUPERBLOCK_V1 || jbt == JBD2_SUPERBLOCK_V2) &&
                be32(jsb->s_blocksize) == out->block_size && !(jinc & jbad)) {
                out->j_first    = be32(jsb->s_first);
                out->j_maxlen   = be32(jsb->s_maxlen);
                out->j_sequence = be32(jsb->s_sequence);
                if (out->j_sequence == 0) out->j_sequence = 1;  /* jbd2 seq >= 1 */
                mm_memcpy(out->j_uuid, jsb->s_uuid, 16);
                out->j_sb_pbn = jpbn;
                out->j_csum3  = (jinc & JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0;
                /* seed for tag/descriptor/commit csums = crc32c(~0, journal uuid) */
                out->j_csum_seed = ext4_crc32c(0xFFFFFFFFu, out->j_uuid, 16);
                if (out->j_maxlen > out->j_first + 3)
                    out->j_enabled = 1;
                /* P7: deferred-checkpoint epoch state.  The log is empty here
                 * (recovery, run earlier, left s_start=0), so the first epoch
                 * starts writing at j_first. */
                s_jhead      = out->j_first;
                s_epoch_open = 0;
                s_ckpt.n     = 0;
                s_force_ckpt = 0;
                s_epoch_seq  = 0;

                /* Make the journal a well-formed csum-v3 journal on a
                 * metadata_csum fs, like the Linux kernel does on first rw mount.
                 * Fires when the journal is still plain OR was left half-upgraded
                 * (CSUM_V3 set but s_checksum_type unset — jbd2/e2fsck reject
                 * that).  Only with a CLEAN log (s_start==0, true after recovery /
                 * on a clean image) — flipping journal features mid-log would
                 * corrupt replay — and only on a writable mount.  Sets CSUM_V3 +
                 * s_checksum_type=crc32c, clears the deprecated v1 compat-checksum,
                 * and restamps the journal-sb csum (seed ~0).  On persist failure,
                 * fully restore the in-memory sb so later commits never write a
                 * half-upgraded superblock. */
                int type_ok = (out->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF] == JBD2_CRC32C_CHKSUM);
                if (out->j_enabled && out->has_metadata_csum && !out->read_only &&
                    be32(jsb->s_start) == 0 && (!out->j_csum3 || !type_ok)) {
                    int was_v3 = out->j_csum3;
                    uint32_t o_inc  = jsb->s_feature_incompat;   /* be, for revert  */
                    uint32_t o_comp = jsb->s_feature_compat;
                    uint8_t  o_type = out->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF];

                    jsb->s_feature_incompat = __builtin_bswap32(jinc | JBD2_FEATURE_INCOMPAT_CSUM_V3);
                    jsb->s_feature_compat   = __builtin_bswap32(
                        be32(jsb->s_feature_compat) & ~JBD2_FEATURE_COMPAT_CHECKSUM);
                    out->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF] = JBD2_CRC32C_CHKSUM;
                    ext4_jsb_csum_set(out->j_sb_buf, 1);

                    if (jlog_write(out, 0, out->j_sb_buf) == ST_OK) {
                        if (out->bdev && out->bdev->sync)
                            out->bdev->sync((block_device_t *)out->bdev);
                        out->j_csum3 = 1;
                        kprintf("ext4: %s journal to csum-v3 (metadata_csum fs)\n",
                                was_v3 ? "fixed s_checksum_type on" : "upgraded plain");
                    } else {
                        jsb->s_feature_incompat = o_inc;
                        jsb->s_feature_compat   = o_comp;
                        out->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF] = o_type;
                        ext4_jsb_csum_set(out->j_sb_buf, was_v3);  /* restore prior */
                    }
                }
            }
        }
        if (out->j_enabled) {
            kprintf("ext4: journaled writes ON (ordered; journal %lu blocks, next seq %u%s)\n",
                    out->j_maxlen, out->j_sequence,
                    out->j_csum3 ? ", csum-v3" : "");
        } else {
            kprintf("ext4: journaled writes OFF (no usable journal / unsupported journal features)\n");
            if (out->j_sb_buf) { kfree(out->j_sb_buf); out->j_sb_buf = 0; }
        }
    }
    return ST_OK;
}

int ext4_vfs_register_root(ext4_fs_t *fs)
{
    if (!fs)
        return ST_INVALID;
    g_ext4_fs = fs;
    g_ext4_cwd_ino = EXT4_ROOT_INO;
    ext4_sb_attach(fs);
    vfs_register_root(&ext4_vfs_ops);
    return ST_OK;
}
