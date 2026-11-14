// LikeOS - ext4 filesystem driver
//
// A full read/write ext4 driver: extent + indirect block mapping, directory
// traversal, allocation and metadata writeback, symlinks/hard links, journaling
// (ordered mode), metadata_csum (crc32c), xattrs/ACLs, and permission
// enforcement, integrated with the generic page/inode caches.
// Structurally this mirrors kernel/fs/fat32.c: a
// reentrant sleeping I/O mutex serialises operations, the data path runs
// through the shared pagecache, and the two vtables (vfs_ops_t for path/
// handle ops, vfs_sb_ops_t for the cache's block plumbing) are filled in.
//
// The block-chain integration uses self-describing block ids — see the
// big comment in kernel/fs/ext4.h.
//
// Copyright (C) 2026 The LikeOS Project

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
#include <kernel/fs/mount.h>
#include <kernel/fs/kdev_t.h>
#include <kernel/uapi/bug.h>
#include <kernel/dev/block/blk_internal.h>

/* ext4_fs_t.mnt_opts: what a mount(2) asked for beyond its flags. */
/* The journal is not replayed ("norecovery", "noload"); read-only only. */
#define EXT4_MNT_NOLOAD 0x1u
/* A journal that needs replaying on a write-protected device refuses the
 * mount instead of being skipped silently (every mount(2); the root at boot
 * keeps going as it always did). */
#define EXT4_MNT_WP_STRICT 0x2u

/* Max sectors per device transfer (== USB_MSD_MAX_BLOCKS in usb_msd.c).  Also the
 * size of the write-back coalescing buffer (st->wbounce).  2048 sectors = 1 MiB:
 * large transfers amortise the USB per-command latency (the throughput limiter),
 * and the xHCI ring (256 TRBs) chains the needed 16 TRBs comfortably. */
#define EXT4_MAX_SECTORS_PER_READ 2048

/* On-disk layout is load-bearing: verify sizes/offsets at compile time. */
_Static_assert(sizeof(ext4_super_block) == 1024,
	       "ext4_super_block must be 1024 bytes");
_Static_assert(sizeof(ext4_group_desc) == 64,
	       "ext4_group_desc must be 64 bytes");
_Static_assert(sizeof(ext4_extent_header) == 12,
	       "ext4_extent_header must be 12 bytes");
_Static_assert(sizeof(ext4_extent_idx) == 12,
	       "ext4_extent_idx must be 12 bytes");
_Static_assert(sizeof(ext4_extent) == 12, "ext4_extent must be 12 bytes");
_Static_assert(__builtin_offsetof(ext4_super_block, s_magic) == 0x38,
	       "s_magic offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_inode_size) == 0x58,
	       "s_inode_size offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_feature_incompat) == 0x60,
	       "s_feature_incompat offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_desc_size) == 0xFE,
	       "s_desc_size offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_blocks_count_hi) == 0x150,
	       "s_blocks_count_hi offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_links_count) == 0x1A,
	       "i_links_count offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_flags) == 0x20,
	       "i_flags offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_block) == 0x28,
	       "i_block offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_size_high) == 0x6C,
	       "i_size_high offset");
_Static_assert(__builtin_offsetof(ext4_group_desc, bg_inode_table_lo) == 8,
	       "bg_inode_table_lo offset");
_Static_assert(sizeof(ext4_super_block) == 1024,
	       "ext4_super_block must be 1024 bytes");
_Static_assert(__builtin_offsetof(ext4_super_block, s_checksum_type) == 0x175,
	       "s_checksum_type offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_checksum_seed) == 0x270,
	       "s_checksum_seed offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_checksum) == 0x3FC,
	       "s_checksum offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_error_count) == 0x194,
	       "s_error_count offset");
_Static_assert(__builtin_offsetof(ext4_group_desc, bg_checksum) == 0x1E,
	       "bg_checksum offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_checksum_hi) == 0x82,
	       "i_checksum_hi offset");

/* ===================================================================
 * metadata_csum (crc32c, Castagnoli) — checksum machinery.
 *
 * The read-side VERIFICATION path computes the checksums the way the
 * reference does and WARN (once) if an on-disk value disagrees, so the crc32c
 * implementation + seed derivation can be proven against a real image before
 * any write-side code relies on them.  Nothing here rejects or mutates.
 * =================================================================== */

/* crc32c (reflected polynomial 0x82F63B78).  No implicit pre/post inversion —
 * the caller supplies the seed explicitly (matching the reference's usage).
 *
 * Performance-critical: a checksum is verified on every metadata block read
 * (4 KB directory leaves per lookup, extent nodes, inode records) and computed
 * on every journaled block.  The original bit-at-a-time loop cost ~50 µs per
 * 4 KB block, several times per path syscall — a large share of why every
 * stat/open took ~1 ms.  Dispatch: the SSE4.2 CRC32 instruction (which
 * implements exactly this Castagnoli polynomial) when the CPU has it, else a
 * slice-by-4 table (~30x the bitwise speed). */
#define EXT4_C32C_UNINIT 0
#define EXT4_C32C_TABLE 1
#define EXT4_C32C_HW 2
static int s_c32c_mode = EXT4_C32C_UNINIT;
static uint32_t s_c32c_tab[4][256];

static void ext4_crc32c_init(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile("cpuid"
			 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			 : "a"(1), "c"(0));
	if (c & (1u << 20)) { /* CPUID.1:ECX.SSE4_2 */
		s_c32c_mode = EXT4_C32C_HW;
		return;
	}
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t crc = i;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
		s_c32c_tab[0][i] = crc;
	}
	for (uint32_t i = 0; i < 256; i++) {
		s_c32c_tab[1][i] = (s_c32c_tab[0][i] >> 8) ^
				   s_c32c_tab[0][s_c32c_tab[0][i] & 0xFF];
		s_c32c_tab[2][i] = (s_c32c_tab[1][i] >> 8) ^
				   s_c32c_tab[0][s_c32c_tab[1][i] & 0xFF];
		s_c32c_tab[3][i] = (s_c32c_tab[2][i] >> 8) ^
				   s_c32c_tab[0][s_c32c_tab[2][i] & 0xFF];
	}
	/* Publish the mode only after the tables are complete (a concurrent
	 * caller may race the lazy init; rebuilding the same values is safe,
	 * but reading a half-built table is not). */
	__atomic_store_n(&s_c32c_mode, EXT4_C32C_TABLE, __ATOMIC_RELEASE);
}

static uint32_t ext4_crc32c(uint32_t crc, const void *buf, unsigned long len)
{
	const uint8_t *p = (const uint8_t *)buf;
	int mode = __atomic_load_n(&s_c32c_mode, __ATOMIC_ACQUIRE);
	if (mode == EXT4_C32C_UNINIT) {
		ext4_crc32c_init();
		mode = s_c32c_mode;
	}
	if (mode == EXT4_C32C_HW) {
		uint64_t c = crc;
		while (((uintptr_t)p & 7) && len) {
			__asm__("crc32b %1, %0" : "+r"(c) : "rm"(*p));
			p++;
			len--;
		}
		while (len >= 8) {
			__asm__("crc32q %1, %0"
				: "+r"(c)
				: "rm"(*(const uint64_t *)p));
			p += 8;
			len -= 8;
		}
		while (len--) {
			__asm__("crc32b %1, %0" : "+r"(c) : "rm"(*p));
			p++;
		}
		return (uint32_t)c;
	}
	while (((uintptr_t)p & 3) && len) {
		crc = (crc >> 8) ^ s_c32c_tab[0][(crc ^ *p++) & 0xFF];
		len--;
	}
	while (len >= 4) {
		uint32_t w;
		mm_memcpy(&w, p, 4);
		crc ^= w;
		crc = s_c32c_tab[3][crc & 0xFF] ^
		      s_c32c_tab[2][(crc >> 8) & 0xFF] ^
		      s_c32c_tab[1][(crc >> 16) & 0xFF] ^ s_c32c_tab[0][crc >> 24];
		p += 4;
		len -= 4;
	}
	while (len--)
		crc = (crc >> 8) ^ s_c32c_tab[0][(crc ^ *p++) & 0xFF];
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
	uint32_t le_group = group; /* x86 is little-endian */
	uint32_t crc = ext4_crc32c(fs->csum_seed, &le_group, sizeof(le_group));
	crc = ext4_crc32c(crc, gd, off); /* up to bg_checksum         */
	crc = ext4_crc32c(crc, &zero, 2); /* the zeroed bg_checksum     */
	if (fs->desc_size > off + 2) /* rest after the csum field  */
		crc = ext4_crc32c(crc, (const uint8_t *)gd + off + 2,
				  fs->desc_size - (off + 2));
	return (uint16_t)(crc & 0xFFFF);
}

/* Inode checksum over the full on-disk inode bytes `raw` (length inode_size),
 * with the two embedded checksum fields treated as zero.  Returns the 32-bit
 * value; the low 16 bits go in l_i_checksum_lo (i_osd2+8), the high 16 in
 * i_checksum_hi (only when the inode is large enough to hold it). */
#define EXT4_INO_CSUM_LO_OFF 0x7C /* l_i_checksum_lo within i_osd2[8]   */
#define EXT4_INO_CSUM_HI_OFF 0x82 /* i_checksum_hi                      */
#define EXT4_GOOD_OLD_ISIZE 128
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
		mm_memcpy(&extra,
			  raw + __builtin_offsetof(ext4_inode, i_extra_isize),
			  2);
		/* [old_size, i_checksum_hi) */
		crc = ext4_crc32c(crc, raw + EXT4_GOOD_OLD_ISIZE,
				  EXT4_INO_CSUM_HI_OFF - EXT4_GOOD_OLD_ISIZE);
		int has_hi = (extra >=
			      (EXT4_INO_CSUM_HI_OFF + 2) - EXT4_GOOD_OLD_ISIZE);
		if (has_hi)
			crc = ext4_crc32c(crc, &zero, 2); /* zeroed hi */
		else
			crc = ext4_crc32c(crc, raw + EXT4_INO_CSUM_HI_OFF, 2);
		crc = ext4_crc32c(crc, raw + EXT4_INO_CSUM_HI_OFF + 2,
				  fs->inode_size - (EXT4_INO_CSUM_HI_OFF + 2));
	}
	return crc;
}

/* ===================================================================
 * metadata_csum WRITE side — stamp the checksums computed above into
 * the on-disk metadata just before it is written.  Every helper is a no-op
 * unless the filesystem has metadata_csum, so a ^metadata_csum image is written
 * byte-for-byte as before.
 * =================================================================== */

/* Per-inode checksum seed (csum_seed folded with the inode number + its
 * generation).  Shared by the inode and directory-leaf checksums. */
static uint32_t ext4_inode_csum_seed(const ext4_fs_t *fs, unsigned long ino,
				     uint32_t gen)
{
	uint32_t le_ino = (uint32_t)ino;
	uint32_t crc = ext4_crc32c(fs->csum_seed, &le_ino, sizeof(le_ino));
	return ext4_crc32c(crc, &gen, sizeof(gen));
}

/* Stamp the inode checksum into the full on-disk inode bytes `raw`
 * (fs->inode_size long) for inode `ino`.  Mirrors the field-presence rules of
 * ext4_inode_csum (the hi half only when the inode is large enough to hold it). */
static void ext4_inode_csum_set(const ext4_fs_t *fs, unsigned long ino,
				uint8_t *raw)
{
	if (!fs->has_metadata_csum)
		return;
	uint32_t csum = ext4_inode_csum(fs, ino, raw);
	*(uint16_t *)(raw + EXT4_INO_CSUM_LO_OFF) = (uint16_t)(csum & 0xFFFF);
	if (fs->inode_size > EXT4_GOOD_OLD_ISIZE) {
		uint16_t extra;
		mm_memcpy(&extra,
			  raw + __builtin_offsetof(ext4_inode, i_extra_isize),
			  2);
		if (extra >= (EXT4_INO_CSUM_HI_OFF + 2) - EXT4_GOOD_OLD_ISIZE)
			*(uint16_t *)(raw + EXT4_INO_CSUM_HI_OFF) =
				(uint16_t)(csum >> 16);
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
	if (!fs->has_metadata_csum)
		return;
	uint8_t *tail = blk + fs->block_size - EXT4_DIR_TAIL_SIZE;
	tail[0] = tail[1] = tail[2] = tail[3] =
		0; /* det_reserved_zero1 (inode=0)  */
	tail[4] = EXT4_DIR_TAIL_SIZE;
	tail[5] = 0; /* det_rec_len = 12              */
	tail[6] = 0; /* det_reserved_zero2            */
	tail[7] = 0xDE; /* det_reserved_ft (csum marker) */
	uint32_t seed = ext4_inode_csum_seed(fs, dir_ino, gen);
	uint32_t csum =
		ext4_crc32c(seed, blk, fs->block_size - EXT4_DIR_TAIL_SIZE);
	*(uint32_t *)(tail + 8) = csum; /* det_checksum                  */
}

/* Recompute the block / inode bitmap checksums (kept in the group descriptor)
 * after the bitmap buffer `bm` for group `g` is modified.  The descriptor is
 * flushed later by ext4_write_gd, which also recomputes bg_checksum over it.
 * The csum spans clusters_per_group/8 (== blocks_per_group/8, no bigalloc) /
 * (inodes_per_group+7)/8 bytes, exactly as the reference. */
static void ext4_block_bitmap_csum_set(ext4_fs_t *fs, unsigned g,
				       const uint8_t *bm)
{
	if (!fs->has_metadata_csum)
		return;
	uint32_t csum =
		ext4_crc32c(fs->csum_seed, bm, fs->blocks_per_group / 8);
	fs->gdt[g].bg_block_bitmap_csum_lo = (uint16_t)(csum & 0xFFFF);
	if (fs->desc_size >= 64)
		fs->gdt[g].bg_block_bitmap_csum_hi = (uint16_t)(csum >> 16);
}
static void ext4_inode_bitmap_csum_set(ext4_fs_t *fs, unsigned g,
				       const uint8_t *bm)
{
	if (!fs->has_metadata_csum)
		return;
	uint32_t csum =
		ext4_crc32c(fs->csum_seed, bm, (fs->inodes_per_group + 7) / 8);
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
	if (!fs->has_metadata_csum)
		return 1;
	const uint8_t *tail = blk + fs->block_size - EXT4_DIR_TAIL_SIZE;
	uint32_t tino;
	mm_memcpy(&tino, tail, 4);
	uint16_t trec;
	mm_memcpy(&trec, tail + 4, 2);
	if (tino != 0 || trec != EXT4_DIR_TAIL_SIZE || tail[6] != 0 ||
	    tail[7] != 0xDE)
		return 1; /* not a dirent csum tail    */
	uint32_t want;
	mm_memcpy(&want, tail + 8, 4);
	uint32_t seed = ext4_inode_csum_seed(fs, dir_ino, gen);
	return ext4_crc32c(seed, blk, fs->block_size - EXT4_DIR_TAIL_SIZE) ==
	       want;
}

/* Compare a block/inode bitmap buffer with the csum kept in its group
 * descriptor, whatever the group's UNINIT flags say.  Returns 1 if they match
 * (or there is no metadata_csum).  The repair of stale UNINIT flags uses this
 * directly: there the flag is the thing in doubt. */
static int ext4_block_bitmap_csum_match(const ext4_fs_t *fs, unsigned g,
					const uint8_t *bm)
{
	if (!fs->has_metadata_csum)
		return 1;
	uint32_t csum =
		ext4_crc32c(fs->csum_seed, bm, fs->blocks_per_group / 8);
	uint32_t want = fs->gdt[g].bg_block_bitmap_csum_lo;
	if (fs->desc_size >= 64)
		want |= (uint32_t)fs->gdt[g].bg_block_bitmap_csum_hi << 16;
	else
		csum &= 0xFFFFu;
	return csum == want;
}
static int ext4_inode_bitmap_csum_match(const ext4_fs_t *fs, unsigned g,
					const uint8_t *bm)
{
	if (!fs->has_metadata_csum)
		return 1;
	uint32_t csum =
		ext4_crc32c(fs->csum_seed, bm, (fs->inodes_per_group + 7) / 8);
	uint32_t want = fs->gdt[g].bg_inode_bitmap_csum_lo;
	if (fs->desc_size >= 64)
		want |= (uint32_t)fs->gdt[g].bg_inode_bitmap_csum_hi << 16;
	else
		csum &= 0xFFFFu;
	return csum == want;
}

/* Verify a bitmap read for use.  Returns 1 if OK: it matches, there is no
 * metadata_csum, or the group is flagged uninitialized (its bitmap is not
 * laid out on disk yet). */
static int ext4_block_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g,
				     const uint8_t *bm)
{
	if (fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT)
		return 1;
	return ext4_block_bitmap_csum_match(fs, g, bm);
}
static int ext4_inode_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g,
				     const uint8_t *bm)
{
	if (fs->gdt[g].bg_flags & EXT4_BG_INODE_UNINIT)
		return 1;
	return ext4_inode_bitmap_csum_match(fs, g, bm);
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
	if (!fs->has_metadata_csum)
		return 1;
	const ext4_extent_header *eh = (const ext4_extent_header *)blk;
	unsigned long off = sizeof(ext4_extent_header) +
			    (unsigned long)eh->eh_max * sizeof(ext4_extent);
	if (off + 4 > fs->block_size)
		return 1; /* malformed eh_max — not ours  */
	uint32_t want;
	mm_memcpy(&want, blk + off, 4);
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


/* ===================================================================
 * Per-filesystem state
 *
 * Everything a mounted ext4 filesystem keeps besides its geometry
 * (ext4_fs_t): the journal transaction and checkpoint batch, the lock
 * hierarchy, the metadata block cache, scratch buffers, deferred inode
 * updates, the write-back buffer and the ordered-data set.  One per mount,
 * reached as fs->st, allocated by ext4_mount() and released by the unmount,
 * so any number of ext4 filesystems can be mounted side by side and none of
 * them shares a lock or a cached block with another.
 * =================================================================== */
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
	spinlock_t lock; /* protects the fields above              */
} ext4_rwsem_t;

#define EXT4_RWSEM_INIT(name)                                        \
	{                                                            \
		.readers = 0, .writer = 0, .owner = (uint64_t)-1,    \
		.wdepth = 0, .w_wait = 0, .r_wait = 0,               \
		.reader_turn = 0, .lock = SPINLOCK_INIT(name)        \
	}

#define EXT4_ILOCKS 64
#define EXT4_BGLOCKS 32
/* Sized so a metadata-heavy operation's whole working set stays resident: a big
 * directory grows to many leaf/index blocks, and a create/delete loop touches
 * dozens of inode-table + bitmap + dir blocks.  256 * 4 KB = up to 1 MB, buffers
 * allocated on demand.  (Was 32, which thrashed on htree-sized directories.) */
#define EXT4_MBC_ENTRIES 256
#define EXT4_BPOOL 16
#define EXT4_DI_MAX 64
#define EXT4_IC_ENTRIES 8

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
	int epoch_open; /* committed, un-checkpointed txns?    */
	uint32_t epoch_seq; /* sequence of the epoch's first txn   */

	/* The lock hierarchy (see "Fine-grained locking engine"). */
	ext4_rwsem_t meta;
	ext4_rwsem_t ilock[EXT4_ILOCKS];
	ext4_rwsem_t bglock[EXT4_BGLOCKS];

	/* Metadata block cache. */
	struct {
		unsigned long pbn; /* 0 = empty slot               */
		uint8_t *data; /* block_size bytes             */
		unsigned size; /* allocated bytes (for buffer-cache accounting) */
		int verified; /* type-specific csum already checked once */
	} mbc[EXT4_MBC_ENTRIES];
	unsigned mbc_next;
	spinlock_t mbc_lock;

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

/* ---- Inode fields beyond the original 128 bytes ---------------------------
 *
 * An inode larger than 128 bytes carries i_extra_isize more bytes of fields
 * after them; a field there exists only if that size covers it.  The
 * timestamps' _extra words hold the nanoseconds (bits 2..31) and two more
 * bits of the seconds (bits 0..1: the epochs after 2038); the owner's upper
 * 16 bits live in the osd2 area of the base inode (bytes 4..5 uid, 6..7 gid
 * of i_osd2), which every inode has. */
#define EXT4_T_ATIME 1u
#define EXT4_T_MTIME 2u
#define EXT4_T_CTIME 4u

static int ext4_inode_has_extra(const ext4_fs_t *fs, const ext4_inode *in,
				unsigned off)
{
	return fs->inode_size > EXT4_GOOD_OLD_ISIZE &&
	       off + 4 <= fs->inode_size &&
	       (unsigned)EXT4_GOOD_OLD_ISIZE + in->i_extra_isize >= off + 4;
}

static uint32_t ext4_time_extra(int64_t sec, uint32_t nsec)
{
	int64_t hi = (sec - (int64_t)(int32_t)(uint32_t)sec) >> 32;
	return (nsec << 2) | ((uint32_t)hi & 3u);
}

static void ext4_time_decode(uint32_t lo, uint32_t extra, int has_extra,
			     uint64_t *sec, uint64_t *nsec)
{
	int64_t t = (int64_t)(int32_t)lo;

	if (has_extra) {
		t += (int64_t)(extra & 3u) << 32;
		*nsec = extra >> 2;
		if (*nsec > 999999999u)
			*nsec = 0;
	} else {
		*nsec = 0;
	}
	*sec = (uint64_t)t;
}

/* Set the chosen timestamps (EXT4_T_*) to sec.nsec. */
static void ext4_stamp(const ext4_fs_t *fs, ext4_inode *in, unsigned which,
		       int64_t sec, uint32_t nsec)
{
	uint32_t lo = (uint32_t)sec, extra = ext4_time_extra(sec, nsec);

	if (which & EXT4_T_ATIME) {
		in->i_atime = lo;
		if (ext4_inode_has_extra(
			    fs, in, __builtin_offsetof(ext4_inode, i_atime_extra)))
			in->i_atime_extra = extra;
	}
	if (which & EXT4_T_MTIME) {
		in->i_mtime = lo;
		if (ext4_inode_has_extra(
			    fs, in, __builtin_offsetof(ext4_inode, i_mtime_extra)))
			in->i_mtime_extra = extra;
	}
	if (which & EXT4_T_CTIME) {
		in->i_ctime = lo;
		if (ext4_inode_has_extra(
			    fs, in, __builtin_offsetof(ext4_inode, i_ctime_extra)))
			in->i_ctime_extra = extra;
	}
}

/* ...to the current time. */
static void ext4_stamp_now(const ext4_fs_t *fs, ext4_inode *in, unsigned which)
{
	ext4_stamp(fs, in, which, (int64_t)timer_get_epoch(), 0);
}

static uint32_t ext4_inode_uid(const ext4_inode *in)
{
	uint16_t hi;

	mm_memcpy(&hi, in->i_osd2 + 4, 2);
	return (uint32_t)in->i_uid | ((uint32_t)hi << 16);
}

static uint32_t ext4_inode_gid(const ext4_inode *in)
{
	uint16_t hi;

	mm_memcpy(&hi, in->i_osd2 + 6, 2);
	return (uint32_t)in->i_gid | ((uint32_t)hi << 16);
}

static void ext4_inode_set_uid(ext4_inode *in, uint32_t uid)
{
	uint16_t hi = (uint16_t)(uid >> 16);

	in->i_uid = (uint16_t)uid;
	mm_memcpy(in->i_osd2 + 4, &hi, 2);
}

static void ext4_inode_set_gid(ext4_inode *in, uint32_t gid)
{
	uint16_t hi = (uint16_t)(gid >> 16);

	in->i_gid = (uint16_t)gid;
	mm_memcpy(in->i_osd2 + 6, &hi, 2);
}

/* Refuse a mutating op up front when the fs is read-only / error-latched.  The
 * block-write chokepoints (ext4_write_block_direct / ext4_write_impl) already
 * prevent corruption; this just returns a clean -EROFS at entry instead of
 * letting an op do partial work and fail partway. */
static inline int ext4_is_ro(ext4_fs_t *fs)
{
	return fs && fs->read_only;
}

/* ===================================================================
 * Journaled-writes transaction (ordered mode).
 *
 * A transaction spans one top-level operation: it begins on the outermost
 * ext4_io_lock(fs) and commits on the matching outermost ext4_io_unlock(fs).
 * While active, ext4_write_block() buffers the *structural* metadata it would
 * write (inode tables, directory blocks, allocation bitmaps) into st->txn
 * instead of writing it in place; commit then journals the whole set
 * atomically (descriptor + blocks + commit) and checkpoints it to the final
 * locations.  Data blocks are written in place during the op (ordered mode),
 * so committed metadata never references unwritten data.  Serialized entirely
 * by ext4_io_lock, so no extra locking is needed for st->txn.
 * =================================================================== */

static inline int ext4_txn_active(ext4_fs_t *fs)
{
	return fs->st->txn.active;
}
static int ext4_write_block_direct(ext4_fs_t *fs, unsigned long pbn,
				   const void *buf);
static void ext4_txn_flush(ext4_fs_t *fs); /* defined with the journal code */
static void
ext4_journal_clean(ext4_fs_t *fs); /* mark journal empty on sync     */
/* Record that this transaction wrote data into `ino`, so the commit stores
 * that data before the metadata describing it.  Defined with the journal. */
static void ext4_ordered_add(ext4_fs_t *fs, unsigned long ino);
static int ext4_wb_flush(ext4_fs_t *fs); /* flush the data write-back buffer */
/* Record a metadata-corruption error + apply the errors=
 * policy (remount-ro latch / panic / continue).  Defined after ext4_write_super. */
static void ext4_fs_error(ext4_fs_t *fs, const char *what, unsigned long ino);

/* Begin a transaction (called on the outermost ext4_io_lock acquire). */
static void ext4_txn_begin(ext4_fs_t *fs)
{
	if (!fs->j_enabled)
		return;
	fs->st->txn.active = 1;
	fs->st->txn.n = 0;
}

/* Buffer (or overwrite) a metadata block in the running transaction. */
static void ext4_txn_capture(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
	/* A zero pbn here would be journaled as a block-0 tag and skipped (with a
	 * warning) on replay — catch the writer at the source instead. */
	WARN_ON_ONCE(pbn == 0);
	for (unsigned i = 0; i < fs->st->txn.n; i++)
		if (fs->st->txn.blk[i] == pbn) { /* re-dirtied in same op   */
			mm_memcpy(fs->st->txn.data[i], buf, fs->block_size);
			return;
		}
	if (fs->st->txn.n == fs->st->txn.cap) { /* grow the pointer arrays */
		unsigned nc = fs->st->txn.cap ? fs->st->txn.cap * 2 : 16;
		unsigned long *nb =
			(unsigned long *)kalloc(nc * sizeof(unsigned long));
		uint8_t **nd = (uint8_t **)kalloc(nc * sizeof(uint8_t *));
		if (!nb || !nd) { /* OOM: degrade to direct  */
			if (nb)
				kfree(nb);
			if (nd)
				kfree(nd);
			WARN_ON_ONCE(1);
			ext4_write_block_direct(fs, pbn, buf);
			return;
		}
		for (unsigned i = 0; i < fs->st->txn.n; i++) {
			nb[i] = fs->st->txn.blk[i];
			nd[i] = fs->st->txn.data[i];
		}
		for (unsigned i = fs->st->txn.n; i < nc; i++)
			nd[i] = 0; /* alloc on demand */
		if (fs->st->txn.blk)
			kfree(fs->st->txn.blk);
		if (fs->st->txn.data)
			kfree(fs->st->txn.data);
		fs->st->txn.blk = nb;
		fs->st->txn.data = nd;
		fs->st->txn.cap = nc;
	}
	if (!fs->st->txn.data[fs->st->txn.n])
		fs->st->txn.data[fs->st->txn.n] = (uint8_t *)kalloc(fs->block_size);
	if (!fs->st->txn.data[fs->st->txn.n]) {
		WARN_ON_ONCE(1);
		ext4_write_block_direct(fs, pbn, buf);
		return;
	}
	mm_memcpy(fs->st->txn.data[fs->st->txn.n], buf, fs->block_size);
	fs->st->txn.blk[fs->st->txn.n] = pbn;
	fs->st->txn.n++;
}

/* Read-your-writes: serve a block from the running transaction if buffered. */
static int ext4_txn_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	for (unsigned i = 0; i < fs->st->txn.n; i++)
		if (fs->st->txn.blk[i] == pbn) {
			mm_memcpy(buf, fs->st->txn.data[i], fs->block_size);
			return 1;
		}
	return 0;
}

/* ===================================================================
 * Deferred-checkpoint circular journal log.
 *
 * Instead of checkpointing (writing committed metadata to its final location)
 * synchronously after every op — which cost a sync per op — committed
 * transactions accumulate as an "epoch" in the journal log and are checkpointed
 * lazily (when the log/pending set fills, on fsync/sync, or when an op frees a
 * block that still has a journal copy).  This drops the steady-state cost toward
 * ONE device sync per op (the commit), amortising the 2-sync checkpoint over the
 * whole epoch.
 *
 * st->ckpt is the in-memory authoritative copy of every metadata block written but
 * not yet checkpointed this epoch (latest content per block).  It serves two
 * roles: (1) read-your-writes — until checkpoint, a block's final disk location
 * still holds pre-epoch content, so reads must come from here; (2) the source
 * for the eventual checkpoint writeback.  Bounded by EXT4_CKPT_MAX_BLOCKS.
 *
 * Revoke records are NOT needed, and no checkpoint is forced either: freeing a
 * block drops its journal copy from st->txn / st->ckpt (ext4_drop_journal_copy).
 * Both live only in memory until ext4_journal_flush writes the batch, so a copy
 * that is forgotten was never on disk and can never be replayed over the
 * block's next use -- the same guarantee a revoke gives, without the I/O.
 * Serialized entirely by ext4_io_lock, like st->txn.
 * =================================================================== */
#define EXT4_CKPT_MAX_BLOCKS 256 /* pending unique-block cap (memory)  */
/* Transaction batching.  Each op's metadata is merged into the in-memory
 * batch (st->ckpt) on commit WITHOUT touching the disk; the batch is journalled +
 * checkpointed as ONE transaction only when it reaches this many distinct blocks,
 * on fsync/sync/unmount, or when a journalled block is freed.  This is what makes
 * the driver fast: the per-op device sync (a cache flush — brutally slow on USB)
 * is amortised over a whole batch of operations, like a real journal's ~5s commit
 * interval.  Between flushes the on-disk log is EMPTY, so a normal crash replays
 * nothing (instant recovery); only a crash during the brief flush window replays,
 * and at most this many blocks — keep it modest so that rare replay stays fast. */
#define EXT4_JOURNAL_BATCH 64
/* (legacy circular-log epoch bound — superseded by EXT4_JOURNAL_BATCH) */
#define EXT4_EPOCH_MAX_BLOCKS 32
static void ext4_checkpoint(ext4_fs_t *fs);

/* Merge one block into the epoch pending set (latest content wins). */
static void ext4_ckpt_merge(ext4_fs_t *fs, unsigned long pbn, const void *buf)
{
	WARN_ON_ONCE(pbn == 0); /* see ext4_txn_capture */
	for (unsigned i = 0; i < fs->st->ckpt.n; i++)
		if (fs->st->ckpt.blk[i] == pbn) {
			mm_memcpy(fs->st->ckpt.data[i], buf, fs->block_size);
			return;
		}
	if (fs->st->ckpt.n == fs->st->ckpt.cap) {
		unsigned nc = fs->st->ckpt.cap ? fs->st->ckpt.cap * 2 : 32;
		unsigned long *nb =
			(unsigned long *)kalloc(nc * sizeof(unsigned long));
		uint8_t **nd = (uint8_t **)kalloc(nc * sizeof(uint8_t *));
		if (!nb || !nd) { /* OOM: checkpoint now to drain, then
                                           * write this block direct as a fallback */
			if (nb)
				kfree(nb);
			if (nd)
				kfree(nd);
			WARN_ON_ONCE(1);
			ext4_write_block_direct(fs, pbn, buf);
			return;
		}
		for (unsigned i = 0; i < fs->st->ckpt.n; i++) {
			nb[i] = fs->st->ckpt.blk[i];
			nd[i] = fs->st->ckpt.data[i];
		}
		for (unsigned i = fs->st->ckpt.n; i < nc; i++)
			nd[i] = 0;
		if (fs->st->ckpt.blk)
			kfree(fs->st->ckpt.blk);
		if (fs->st->ckpt.data)
			kfree(fs->st->ckpt.data);
		fs->st->ckpt.blk = nb;
		fs->st->ckpt.data = nd;
		fs->st->ckpt.cap = nc;
	}
	if (!fs->st->ckpt.data[fs->st->ckpt.n])
		fs->st->ckpt.data[fs->st->ckpt.n] = (uint8_t *)kalloc(fs->block_size);
	if (!fs->st->ckpt.data[fs->st->ckpt.n]) {
		WARN_ON_ONCE(1);
		ext4_write_block_direct(fs, pbn, buf);
		return;
	}
	mm_memcpy(fs->st->ckpt.data[fs->st->ckpt.n], buf, fs->block_size);
	fs->st->ckpt.blk[fs->st->ckpt.n] = pbn;
	fs->st->ckpt.n++;
}

/* Read-your-writes across the epoch: serve a committed-but-not-checkpointed
 * block (its final disk location is still stale until checkpoint). */
static int ext4_ckpt_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	for (unsigned i = 0; i < fs->st->ckpt.n; i++)
		if (fs->st->ckpt.blk[i] == pbn) {
			mm_memcpy(buf, fs->st->ckpt.data[i], fs->block_size);
			return 1;
		}
	return 0;
}

/*
 * Drop any in-memory journal copy of a block that is being FREED.
 *
 * This is what a jbd2 revoke record is for, obtained here for nothing: both
 * st->txn (the running transaction) and st->ckpt (the committed-but-not-yet-
 * checkpointed batch) live only in memory -- ext4_journal_flush() writes the
 * batch to the log, checkpoints it to its final locations, empties the log and
 * resets st->ckpt.n to 0 in one go.  So a copy sitting in either of them has
 * never reached the disk, and simply forgetting it means there is nothing that
 * could ever be replayed over the block's next use.
 *
 * What this replaces: setting a force-checkpoint flag, which made the very next
 * commit do a full durable journal flush -- log write, checkpoint, two device
 * syncs -- on the spot.  Deleting a file typically frees blocks written moments
 * earlier and therefore still in the batch, so EVERY unlink forced one.  Claws
 * Mail removes its cache and temporary files as it exits, which is why exiting
 * took five to seven seconds of USB round trips.
 *
 * The data buffers are owned by the arrays and reused across flushes (see
 * ext4_ckpt_merge: a slot allocates only if its pointer is NULL), so removal
 * swaps the entry with the last one and keeps BOTH buffers -- overwriting the
 * pointer would leak a block-sized allocation per freed block.
 */
static void ext4_drop_journal_copy(ext4_fs_t *fs, unsigned long pbn)
{
	for (unsigned i = 0; i < fs->st->txn.n; i++) {
		if (fs->st->txn.blk[i] != pbn)
			continue;
		uint8_t *keep = fs->st->txn.data[i];

		fs->st->txn.blk[i] = fs->st->txn.blk[fs->st->txn.n - 1];
		fs->st->txn.data[i] = fs->st->txn.data[fs->st->txn.n - 1];
		fs->st->txn.data[fs->st->txn.n - 1] = keep;
		fs->st->txn.n--;
		break;
	}
	for (unsigned i = 0; i < fs->st->ckpt.n; i++) {
		if (fs->st->ckpt.blk[i] != pbn)
			continue;
		uint8_t *keep = fs->st->ckpt.data[i];

		fs->st->ckpt.blk[i] = fs->st->ckpt.blk[fs->st->ckpt.n - 1];
		fs->st->ckpt.data[i] = fs->st->ckpt.data[fs->st->ckpt.n - 1];
		fs->st->ckpt.data[fs->st->ckpt.n - 1] = keep;
		fs->st->ckpt.n--;
		break;
	}
}

/* ===================================================================
 * Fine-grained locking engine.
 *
 * Replaces the single reentrant I/O mutex (which serialised EVERY
 * filesystem operation — including multi-millisecond USB data transfers —
 * against every other one) with a conventional lock hierarchy:
 *
 *   per-inode rwsem  →  metadata rwsem  →  per-block-group lock
 *        →  cache spinlocks (mbc / parsed-inode / bpool / pagecache)
 *        →  block-device mutex
 *
 *   - st->meta (metadata/transaction rwsem): SHARED for read-only metadata
 *     access (path resolution, extent mapping, stat, readdir, xattr
 *     reads); EXCLUSIVE for every mutator.  The exclusive mode is exactly
 *     the old global mutex: the journal transaction machinery (st->txn,
 *     st->ckpt, write-back buffer, group-descriptor dirty spans, superblock
 *     counters) is entirely serialised by it and needs no further change.
 *     Exclusive acquire/release keeps the txn begin/commit hooks.
 *   - st->ilock[] (per-inode rwsems, hashed by inode number): file-content
 *     fences.  Readers hold SHARED across data-page reads (which run with
 *     NO metadata lock at all — see pagecache lock_map); write/truncate/
 *     unlink/rename-overwrite/O_TRUNC hold EXCLUSIVE so a file's data
 *     blocks can never be freed or rewritten under an in-flight reader.
 *     A hash collision merely over-serialises two unrelated files.
 *   - st->bglock[] (per-block-group locks, hashed): serialise allocation
 *     bitmap read-modify-write per group.  Today every allocator also
 *     holds st->meta exclusive (journaled bitmap writes), so these add
 *     ordering structure rather than parallelism; they become live when
 *     allocation moves off the exclusive path.
 *   - Superblock/GDT counter updates: under st->meta exclusive (this is the
 *     filesystem-wide "superblock lock" level of the hierarchy).
 *
 * Fairness: writers set w_wait, and new readers defer to queued writers
 * so a stream of readers cannot starve mutators.  The one exception is a
 * task that already holds a shared fs lock (task->fs_rdepth > 0): its
 * nested shared acquisition (page-in during a read) must not queue
 * behind a writer that is itself waiting for the first shared hold to
 * drain — that would deadlock.  Recursion under one's own EXCLUSIVE hold
 * (write path faulting into a mapped file, nested VFS calls) is granted
 * as a depth increment, preserving the old reentrant-mutex semantics.
 * =================================================================== */

/* Park the current task on `sem`'s wait channel.  Same discipline as the
 * other sleeping fs/device mutexes: blind-block under the spinlock, then
 * schedule; the wake side (sched_wake_channel) claims BLOCKED→READY
 * atomically, and sched_schedule handles the woken-before-scheduled race. */
static void ext4_rwsem_park(ext4_rwsem_t *sem, uint64_t *flags)
{
	task_t *cur = sched_current();
	if (cur) {
		cur->wait_channel = (void *)sem;
		task_set_blocked(cur);
	}
	spin_unlock_irqrestore(&sem->lock, *flags);
	sched_schedule();
}

static void ext4_rwsem_read_lock(ext4_rwsem_t *sem)
{
	might_sleep();
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	int queued = 0;
	while (1) {
		uint64_t flags;
		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == my_id && cur) {
			/* Nested acquisition under our own exclusive hold:
			 * treat as exclusive recursion. */
			sem->wdepth++;
			cur->fs_held++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		/* Defer to queued writers -- unless it is this cohort's turn,
		 * or deferring would deadlock a nested shared acquisition. */
		int defer_to_writers = sem->w_wait && !sem->reader_turn &&
				       !(cur && cur->fs_rdepth);
		if (!sem->writer && !defer_to_writers) {
			sem->readers++;
			if (cur) {
				cur->fs_rdepth++;
				cur->fs_held++;
			}
			if (queued) {
				sem->r_wait--;
				/* Cohort drained: the phase is over and the
				 * queued writers go next. */
				if (sem->r_wait == 0)
					sem->reader_turn = 0;
			}
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		if (!queued) {
			sem->r_wait++;
			queued = 1;
		}
		ext4_rwsem_park(sem, &flags);
	}
}

static void ext4_rwsem_read_unlock(ext4_rwsem_t *sem)
{
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	uint64_t flags;
	spin_lock_irqsave(&sem->lock, &flags);
	if (sem->writer && sem->owner == my_id && cur) {
		/* matching nested-under-exclusive acquisition */
		WARN_ON(sem->wdepth <= 1);
		sem->wdepth--;
		if (cur->fs_held > 0)
			cur->fs_held--;
		spin_unlock_irqrestore(&sem->lock, flags);
		return;
	}
	WARN_ON(sem->readers <= 0);
	sem->readers--;
	if (cur && cur->fs_rdepth > 0)
		cur->fs_rdepth--;
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	int wake = (sem->readers == 0);
	spin_unlock_irqrestore(&sem->lock, flags);
	if (wake)
		sched_wake_channel((void *)sem);
}

static void ext4_rwsem_write_lock(ext4_rwsem_t *sem)
{
	might_sleep();
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	int queued = 0;
	while (1) {
		uint64_t flags;
		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == my_id) {
			sem->wdepth++; /* reentrant exclusive */
			if (queued)
				sem->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		/* A cohort of readers was promised this phase; grabbing the
		 * sem in the gap before they wake would re-starve them (the
		 * wake is not instantaneous, and a writer that slips in here
		 * puts the whole stream ahead of them again). */
		int reader_phase = sem->reader_turn && sem->r_wait > 0;
		if (!sem->writer && sem->readers == 0 && !reader_phase) {
			sem->writer = 1;
			sem->owner = my_id;
			sem->wdepth = 1;
			if (queued)
				sem->w_wait--;
			if (cur)
				cur->fs_held++;
			spin_unlock_irqrestore(&sem->lock, flags);
			return;
		}
		if (!queued) {
			sem->w_wait++;
			queued = 1;
		}
		ext4_rwsem_park(sem, &flags);
	}
}

static void ext4_rwsem_write_unlock(ext4_rwsem_t *sem)
{
	task_t *cur = sched_current();
	uint64_t flags;
	spin_lock_irqsave(&sem->lock, &flags);
	WARN_ON(!sem->writer || sem->wdepth <= 0);
	if (cur && cur->fs_held > 0)
		cur->fs_held--;
	if (sem->wdepth > 1) {
		sem->wdepth--;
		spin_unlock_irqrestore(&sem->lock, flags);
		return;
	}
	sem->writer = 0;
	sem->owner = (uint64_t)-1;
	sem->wdepth = 0;
	/* Waiting readers get the next turn, ahead of any queued writer --
	 * see reader_turn in the struct. */
	if (sem->r_wait > 0)
		sem->reader_turn = 1;
	spin_unlock_irqrestore(&sem->lock, flags);
	sched_wake_channel((void *)sem);
}

/* --- the lock instances ------------------------------------------------ */

/* Serializes the function-scope STATIC xattr parse scratch lists in
 * ext4_xattr_get_ino / ext4_xattr_list_ino.  Those lists are static because
 * ext4_xa_list is ~8.6 KB — far too big for the 16 KB kernel stack — which was
 * safe under the old global ext4 mutex but NOT under the fine-grained locking:
 * getxattr/listxattr are read ops holding st->meta only SHARED, so two
 * concurrent ACL/xattr reads parsed into and freed the SAME static list —
 * double-free of the value buffers (SLAB "Double free ... size=32" via
 * ext4_xa_list_free) plus wrong xattr bytes returned to one caller (ACL grant
 * "vanishing" with a clean on-disk fs).  Sleeping lock, NOT a spinlock:
 * ext4_xa_list_free → kfree can TLB-shootdown.  Order: st->meta → s_xa_scratch,
 * never the reverse.  setxattr's static lists need no lock — mutators hold
 * st->meta EXCLUSIVE, which already excludes readers and each other. */
static ext4_rwsem_t s_xa_scratch = EXT4_RWSEM_INIT("ext4_xa");


static void ext4_rwsem_init(ext4_rwsem_t *sem, const char *name)
{
	sem->readers = 0;
	sem->writer = 0;
	sem->owner = (uint64_t)-1;
	sem->wdepth = 0;
	sem->w_wait = 0;
	sem->r_wait = 0;
	sem->reader_turn = 0;
	spinlock_init(&sem->lock, name);
}


/* Debug mirrors read by the scheduler's Ctrl+D dump (legacy names).  They
 * describe the root filesystem's metadata lock. */
volatile int ext4_io_locked = 0;
volatile int ext4_io_depth = 0;
volatile uint64_t ext4_io_owner = (uint64_t)-1;

/* Metadata lock, EXCLUSIVE (mutators).  Semantically identical to the old
 * global mutex including the transaction hooks: the outermost acquire
 * begins a journal transaction, the matching outermost release commits it. */
static void ext4_io_lock(ext4_fs_t *fs)
{
	ext4_rwsem_write_lock(&fs->st->meta);
	if (fs->st->meta.wdepth == 1) {
		ext4_txn_begin(fs);
		if (fs->st->is_root) {
			ext4_io_locked = 1;
			ext4_io_owner = fs->st->meta.owner;
		}
	}
	if (fs->st->is_root)
		ext4_io_depth = fs->st->meta.wdepth;
}

static void ext4_io_unlock(ext4_fs_t *fs)
{
	/* On the outermost release, commit the operation's transaction while
	 * the lock is still held.  The flush sleeps on disk I/O — fine, we
	 * still own the exclusive hold. */
	if (fs->st->meta.wdepth == 1 && fs->st->txn.active)
		ext4_txn_flush(fs);
	if (fs->st->is_root) {
		if (fs->st->meta.wdepth == 1) {
			ext4_io_locked = 0;
			ext4_io_owner = (uint64_t)-1;
			ext4_io_depth = 0;
		} else {
			ext4_io_depth = fs->st->meta.wdepth - 1;
		}
	}
	ext4_rwsem_write_unlock(&fs->st->meta);
}

/* Metadata lock, SHARED (read-only ops: resolve/stat/readdir/extent
 * mapping/xattr reads).  Concurrent with other shared holders. */
static void ext4_meta_rlock(ext4_fs_t *fs)
{
	ext4_rwsem_read_lock(&fs->st->meta);
}
static void ext4_meta_runlock(ext4_fs_t *fs)
{
	ext4_rwsem_read_unlock(&fs->st->meta);
}

/* Does the current task hold the metadata lock EXCLUSIVE?  Used by shared
 * paths that would otherwise perform an opportunistic mutator action
 * (e.g. open's deferred GDT flush). */
static int ext4_meta_held_excl(ext4_fs_t *fs)
{
	task_t *cur = sched_current();
	return fs->st->meta.writer && cur && fs->st->meta.owner == cur->id;
}

/* Per-inode locks. */
static inline ext4_rwsem_t *ext4_ilock_of(ext4_fs_t *fs, unsigned long ino)
{
	return &fs->st->ilock[ino % EXT4_ILOCKS];
}
static void ext4_ilock_shared(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_read_lock(ext4_ilock_of(fs, ino));
}
static void ext4_iunlock_shared(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_read_unlock(ext4_ilock_of(fs, ino));
}
static void ext4_ilock_excl(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_write_lock(ext4_ilock_of(fs, ino));
}
static void ext4_iunlock_excl(ext4_fs_t *fs, unsigned long ino)
{
	ext4_rwsem_write_unlock(ext4_ilock_of(fs, ino));
}

/* Per-block-group allocation locks (nest inside st->meta exclusive). */
static void ext4_bg_lock(ext4_fs_t *fs, unsigned g)
{
	ext4_rwsem_write_lock(&fs->st->bglock[g % EXT4_BGLOCKS]);
}
static void ext4_bg_unlock(ext4_fs_t *fs, unsigned g)
{
	ext4_rwsem_write_unlock(&fs->st->bglock[g % EXT4_BGLOCKS]);
}

/* Crash-abandon cleanup: force-release every lock the dead task still
 * owns in EXCLUSIVE mode.  (Shared holds cannot be attributed to a task
 * without per-task tracking; they also cannot be abandoned in practice —
 * fatal signals defer to delivery points, so a shared holder always runs
 * its unlock path.  Exclusive holds are tracked by owner and swept here.) */
static int ext4_io_release_if_owner(ext4_fs_t *fs, uint64_t task_id)
{
	int released = 0;

	uint64_t flags;
	spin_lock_irqsave(&fs->st->meta.lock, &flags);
	if (fs->st->meta.writer && fs->st->meta.owner == task_id) {
		fs->st->meta.writer = 0;
		fs->st->meta.owner = (uint64_t)-1;
		fs->st->meta.wdepth = 0;
		/* The dead task may have been mid-operation: discard its
		 * uncommitted transaction.  Nothing was journaled or
		 * checkpointed, so the fs stays at its pre-operation state
		 * (atomicity).  Any committed epoch (st->ckpt + log) is durable
		 * and stays — it is checkpointed/replayed normally. */
		fs->st->txn.active = 0;
		fs->st->txn.n = 0;
		if (fs->st->is_root) {
			ext4_io_locked = 0;
			ext4_io_owner = (uint64_t)-1;
			ext4_io_depth = 0;
		}
		released = 1;
	}
	spin_unlock_irqrestore(&fs->st->meta.lock, flags);
	if (released)
		sched_wake_channel((void *)&fs->st->meta);

	for (int i = 0; i < EXT4_ILOCKS; i++) {
		ext4_rwsem_t *sem = &fs->st->ilock[i];
		int r = 0;
		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == task_id) {
			sem->writer = 0;
			sem->owner = (uint64_t)-1;
			sem->wdepth = 0;
			r = 1;
		}
		spin_unlock_irqrestore(&sem->lock, flags);
		if (r) {
			sched_wake_channel((void *)sem);
			released = 1;
		}
	}
	for (int i = 0; i < EXT4_BGLOCKS; i++) {
		ext4_rwsem_t *sem = &fs->st->bglock[i];
		int r = 0;
		spin_lock_irqsave(&sem->lock, &flags);
		if (sem->writer && sem->owner == task_id) {
			sem->writer = 0;
			sem->owner = (uint64_t)-1;
			sem->wdepth = 0;
			r = 1;
		}
		spin_unlock_irqrestore(&sem->lock, flags);
		if (r) {
			sched_wake_channel((void *)sem);
			released = 1;
		}
	}
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
		unsigned long chunk = (count > EXT4_MAX_SECTORS_PER_READ) ?
					      EXT4_MAX_SECTORS_PER_READ :
					      count;
		int st = bdev->read((block_device_t *)bdev, lba, chunk,
				    (uint8_t *)buf + off);
		if (st != ST_OK)
			return st;
		lba += chunk;
		off += chunk * ss;
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
 * NOTE: entries are invalidated on writes. */
/* Sized so a metadata-heavy operation's whole working set stays resident: a big
 * directory grows to many leaf/index blocks, and a create/delete loop touches
 * dozens of inode-table + bitmap + dir blocks.  256 * 4 KB = up to 1 MB, buffers
 * allocated on demand.  (Was 32, which thrashed on htree-sized directories.) */
/* The metadata block cache is read AND repopulated by concurrent shared-mode
 * readers (mutators hold the metadata lock exclusive, but reader–reader
 * insertions race), so it carries its own spinlock.  Copies in/out happen
 * under the lock (a block-size memcpy — sub-microsecond). */

/* ---- checksum-verification cache ----
 * Metadata checksums (directory leaves, extent nodes, xattr blocks) were
 * re-verified on EVERY read of the block — a directory leaf is re-checked on
 * every lookup that scans it, so hot directories paid a full-block crc32c per
 * path component per syscall.  The block cache remembers when a cached copy
 * has already passed its type-specific check; the flag is cleared whenever the
 * cached content changes (fresh disk read, write-through update). */
static int ext4_mbc_verified(ext4_fs_t *fs, unsigned long pbn)
{
	uint64_t flags;
	int v = 0;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (fs->st->mbc[i].pbn == pbn) {
			v = fs->st->mbc[i].verified;
			break;
		}
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	return v;
}

static void ext4_mbc_mark_verified(ext4_fs_t *fs, unsigned long pbn)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	/* All matches, not just the first — symmetric with ext4_mbc_drop, so a
	 * duplicate entry (should no longer exist; insert dedupes) can never
	 * diverge in verified-state from the copy lookups serve. */
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (fs->st->mbc[i].pbn == pbn)
			fs->st->mbc[i].verified = 1;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
}

static void ext4_mbc_invalidate(ext4_fs_t *fs)
{
	/* Detach the buffers under the lock, free them after releasing it —
	 * kfree of a block-sized buffer does a TLB shootdown and must not run
	 * with IRQs off under a spinlock. */
	uint8_t *bufs[EXT4_MBC_ENTRIES];
	uint64_t flags;
	long freed = 0;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
		fs->st->mbc[i].pbn = 0;
		bufs[i] = fs->st->mbc[i].data;
		fs->st->mbc[i].data = 0;
		freed += fs->st->mbc[i].size;
		fs->st->mbc[i].size = 0;
	}
	fs->st->mbc_next = 0;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	mm_buffercache_account(-freed);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (bufs[i])
			kfree(bufs[i]);
}

/* Drop a single block from the metadata cache (on free, so a later reuse of
 * the same physical block never serves stale cached content). */
static void ext4_mbc_drop(ext4_fs_t *fs, unsigned long pbn)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
		if (fs->st->mbc[i].pbn == pbn)
			fs->st->mbc[i].pbn = 0;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
}

/* Serve `pbn` from the cache into `buf`.  Returns 1 on hit. */
static int ext4_mbc_lookup(ext4_fs_t *fs, unsigned long pbn, void *buf)
{
	uint64_t flags;
	int hit = 0;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
		if (fs->st->mbc[i].pbn == pbn && fs->st->mbc[i].data) {
			mm_memcpy(buf, fs->st->mbc[i].data, fs->block_size);
			hit = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
	return hit;
}

/* Insert a freshly-read block.  `verified` seeds the csum-checked flag. */
static void ext4_mbc_insert(ext4_fs_t *fs, unsigned long pbn, const void *buf,
			    int verified)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->mbc_lock, &flags);
	/* DEDUPE — never create a second entry for the same pbn.  Two
	 * shared-mode readers (fine-grained locking allows concurrent readers)
	 * can both miss the same block, both sit in the sleeping disk read, and
	 * both arrive here.  Blind slot assignment then caches the block TWICE.
	 * Duplicates are fatal in slow motion: lookup and the write-side refresh
	 * in ext4_write_block_direct touch only the FIRST match, so the second
	 * copy keeps its old content; once round-robin reuse evicts the first
	 * (fresh) copy, the stale duplicate becomes authoritative, and the next
	 * inode/bitmap RMW through it durably REVERTS every update made since
	 * the duplicate was created (observed: /tmp htree leaf loss + ~78 lost
	 * links_count increments + lost bitmap bits, e2fsck).  Update the
	 * existing entry in place instead.
	 *
	 * INVARIANT: both racing readers read the same disk content (anything
	 * newer lives in st->txn/st->ckpt, checked before the mbc; mutators hold
	 * the meta lock exclusive), so an existing entry MUST equal what we
	 * just read.  A mismatch means a stale/incoherent cached metadata
	 * block — the corruption, caught at the moment it exists. */
	for (int i = 0; i < EXT4_MBC_ENTRIES; i++) {
		if (fs->st->mbc[i].pbn == pbn && fs->st->mbc[i].data) {
			WARN_RATELIMIT(
				ext4_memcmp(fs->st->mbc[i].data, buf,
					    fs->block_size) != 0,
				"ext4 mbc: cached pbn %lu differs from disk (stale metadata cache entry — corruption)",
				pbn);
			mm_memcpy(fs->st->mbc[i].data, buf, fs->block_size);
			fs->st->mbc[i].verified = verified;
			spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
			return;
		}
	}
	unsigned slot = fs->st->mbc_next++ % EXT4_MBC_ENTRIES;
	if (!fs->st->mbc[slot].data) {
		/* One-time buffer allocation for this slot.  kalloc's
		 * allocation path takes only spinlocks (no sleep, no TLB
		 * shootdown — that is on the free path), so this is safe
		 * with IRQs off; it happens once per slot per mount. */
		fs->st->mbc[slot].data = (uint8_t *)kalloc(fs->block_size);
		if (!fs->st->mbc[slot].data) {
			spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
			return;
		}
		fs->st->mbc[slot].size = fs->block_size;
		mm_buffercache_account((long)fs->block_size);
	}
	mm_memcpy(fs->st->mbc[slot].data, buf, fs->block_size);
	fs->st->mbc[slot].pbn = pbn;
	fs->st->mbc[slot].verified = verified;
	spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
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
	/* st->txn/st->ckpt are only mutated under the metadata lock held EXCLUSIVE;
	 * every caller of this function holds it at least SHARED, so these
	 * lookups need no further locking.  The mbc is repopulated by
	 * concurrent shared readers and locks internally. */
	if (fs->st->txn.active && ext4_txn_lookup(fs, pbn, buf))
		return ST_OK;
	if (fs->st->ckpt.n && ext4_ckpt_lookup(fs, pbn, buf))
		return ST_OK;
	if (ext4_mbc_lookup(fs, pbn, buf))
		return ST_OK;
	unsigned long lba = fs->part_lba_offset + pbn * fs->sectors_per_block;
	int st = ext4_read_sectors(fs->bdev, lba, fs->sectors_per_block, buf);
	if (st != ST_OK)
		return st;
	ext4_mbc_insert(fs, pbn, buf, 0); /* fresh from disk: not yet checked */
	return ST_OK;
}

/* ---- recycled scratch block buffers --------------------------------------
 * A block-sized kalloc is a "large" slab allocation: it takes 2 contiguous
 * physical pages (bitmap scan under a global lock), page-table maps them, and
 * the matching kfree unmaps with a cross-CPU TLB shootdown.  The hot metadata
 * paths (resolve, dir lookup, inode read, xattr/ACL read) each burn several of
 * those per operation, which made every path syscall cost milliseconds.  These
 * scratch buffers are allocated once and recycled instead.  Guarded by a
 * spinlock so early-boot/mount callers outside ext4_io_lock stay safe; when
 * the pool is exhausted (or a remount grew block_size) we fall back to kalloc,
 * and ext4_bput(fs) routes foreign pointers back to kfree. */

static void *ext4_bget(ext4_fs_t *fs)
{
	unsigned want = fs->block_size;
	uint64_t flags;
	spin_lock_irqsave(&fs->st->bpool_lock, &flags);
	for (int i = 0; i < EXT4_BPOOL; i++) {
		if (fs->st->bpool[i].busy)
			continue;
		if (fs->st->bpool[i].data && fs->st->bpool[i].size < want) {
			mm_buffercache_account(-(long)fs->st->bpool[i].size);
			kfree(fs->st->bpool[i].data); /* stale from a smaller mount */
			fs->st->bpool[i].data = 0;
		}
		if (!fs->st->bpool[i].data) {
			spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
			uint8_t *nb = (uint8_t *)kalloc(want);
			if (!nb)
				return 0;
			spin_lock_irqsave(&fs->st->bpool_lock, &flags);
			if (fs->st->bpool[i].busy || fs->st->bpool[i].data) {
				/* raced: someone claimed the slot meanwhile */
				spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
				return nb; /* use it as a foreign buffer */
			}
			fs->st->bpool[i].data = nb;
			fs->st->bpool[i].size = want;
			mm_buffercache_account((long)want);
		}
		fs->st->bpool[i].busy = 1;
		uint8_t *p = fs->st->bpool[i].data;
		spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
		mm_memset(p, 0, want); /* match kalloc's zeroed contract */
		return p;
	}
	spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
	return kalloc(want); /* pool exhausted: degrade gracefully */
}

static void ext4_bput(ext4_fs_t *fs, void *p)
{
	if (!p)
		return;
	uint64_t flags;
	spin_lock_irqsave(&fs->st->bpool_lock, &flags);
	for (int i = 0; i < EXT4_BPOOL; i++)
		if (fs->st->bpool[i].data == (uint8_t *)p) {
			WARN_ON_ONCE(!fs->st->bpool[i].busy);
			fs->st->bpool[i].busy = 0;
			spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
			return;
		}
	spin_unlock_irqrestore(&fs->st->bpool_lock, flags);
	kfree(p); /* fallback allocation */
}

/* ===================================================================
 * Group descriptor + inode reads
 * =================================================================== */

/* ---- Block group geometry ------------------------------------------------
 *
 * Where the superblock copies and the group descriptor table live.  The
 * allocators need this to build the block bitmap of a group whose bitmap was
 * never written (BLOCK_UNINIT): its used blocks are exactly the metadata at
 * the start of the group plus whichever of its own bitmaps / inode table
 * blocks fall inside it.
 *
 * Only the classic layout is described: the descriptor table right behind
 * the superblock (no meta_bg), superblock copies in the groups sparse_super
 * names or in every group (no sparse_super2).  The UNINIT flags are honoured
 * on metadata_csum file systems with that layout (ext4_uninit_honoured); on
 * any other the bitmaps on disk are used as they are. */

#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2 0x0200

static int ext4_uninit_honoured(const ext4_fs_t *fs)
{
	return fs->has_metadata_csum &&
	       !(fs->feature_incompat & EXT4_FEATURE_INCOMPAT_META_BG) &&
	       !(fs->feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2);
}

static unsigned long ext4_group_first_block(const ext4_fs_t *fs, unsigned g)
{
	return fs->first_data_block + (unsigned long)g * fs->blocks_per_group;
}

/* Blocks group `g` covers: blocks_per_group, less for a short last group. */
static unsigned ext4_blocks_in_group(const ext4_fs_t *fs, unsigned g)
{
	if (g == fs->groups_count - 1)
		return (unsigned)(fs->blocks_count -
				  ext4_group_first_block(fs, g));
	return fs->blocks_per_group;
}

/* Blocks of the group descriptor table. */
static unsigned ext4_gdt_blocks(const ext4_fs_t *fs)
{
	unsigned per_block = fs->block_size / fs->desc_size;
	return (fs->groups_count + per_block - 1) / per_block;
}

/* Blocks of one group's inode table. */
static unsigned ext4_itable_blocks(const ext4_fs_t *fs)
{
	return (unsigned)(((unsigned long)fs->inodes_per_group *
				   fs->inode_size +
			   fs->block_size - 1) /
			  fs->block_size);
}

/* 1 if `a` is a power of `b` (b^1, b^2, ...). */
static int ext4_is_power_of(unsigned a, unsigned b)
{
	for (;;) {
		if (a < b)
			return 0;
		if (a == b)
			return 1;
		if (a % b)
			return 0;
		a /= b;
	}
}

/* Does group `g` carry a superblock (the primary or a backup)?  sparse_super:
 * groups 0, 1 and the powers of 3, 5 and 7; without it: every group. */
static int ext4_bg_has_super(const ext4_fs_t *fs, unsigned g)
{
	if (g <= 1 ||
	    !(fs->feature_ro_compat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return 1;
	if (!(g & 1))
		return 0;
	return ext4_is_power_of(g, 3) || ext4_is_power_of(g, 5) ||
	       ext4_is_power_of(g, 7);
}

/* Blocks at the very start of group `g` taken by its superblock copy, its GDT
 * copy and the reserved GDT blocks (online-resize headroom, which follows
 * every GDT copy). */
static unsigned ext4_num_base_meta_blocks(const ext4_fs_t *fs, unsigned g)
{
	if (!ext4_bg_has_super(fs, g))
		return 0;
	return 1 + ext4_gdt_blocks(fs) + fs->sb_copy.s_reserved_gdt_blocks;
}

/* Load the entire group descriptor table into fs->gdt at mount time.  The
 * GDT is read on every inode access, so caching it in memory eliminates a
 * block read per inode lookup. */
static int ext4_load_gdt(ext4_fs_t *fs)
{
	if (fs->gdt) {
		kfree(fs->gdt);
		fs->gdt = 0;
	}
	fs->gdt = (ext4_group_desc *)kalloc(fs->groups_count *
					    sizeof(ext4_group_desc));
	if (!fs->gdt)
		return ST_NOMEM;
	mm_memset(fs->gdt, 0, fs->groups_count * sizeof(ext4_group_desc));

	unsigned long gdt_start = (fs->first_data_block + 1);
	unsigned long per_block = fs->block_size / fs->desc_size;
	uint8_t *buf = (uint8_t *)kalloc(fs->block_size);
	if (!buf) {
		kfree(fs->gdt);
		fs->gdt = 0;
		return ST_NOMEM;
	}

	unsigned copy = fs->desc_size < sizeof(ext4_group_desc) ?
				fs->desc_size :
				sizeof(ext4_group_desc);
	unsigned long cur_blk = (unsigned long)-1;
	for (unsigned int g = 0; g < fs->groups_count; g++) {
		unsigned long blk = gdt_start + g / per_block;
		unsigned long off = (g % per_block) * fs->desc_size;
		if (blk != cur_blk) {
			if (ext4_read_sectors(
				    fs->bdev,
				    fs->part_lba_offset +
					    blk * fs->sectors_per_block,
				    fs->sectors_per_block, buf) != ST_OK) {
				kfree(buf);
				kfree(fs->gdt);
				fs->gdt = 0;
				return ST_IO;
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

static unsigned long ext4_inode_table_block(ext4_fs_t *fs,
					    const ext4_group_desc *gd)
{
	unsigned long lo = gd->bg_inode_table_lo;
	unsigned long hi = (fs->desc_size >= 64) ? gd->bg_inode_table_hi : 0;
	return lo | (hi << 32);
}

/* Read raw on-disk inode `ino` into `out`.  Also returns, when requested, the
 * physical block + byte offset of the inode (for O(1) writeback later). */
/* ---- Deferred inode updates ---------------------------------------------
 *
 * A write() changes two things about an inode that nothing else does: its
 * size and its timestamps.  Storing them is a read-modify-write of a
 * 4KB inode-table block through the journal, and it used to happen on every
 * single write() call -- so a program writing a file in 700-byte pieces paid
 * for one per piece, over and over, against the same block.
 *
 * The conventional answer is to keep the change in memory and store it later,
 * in one go.  What makes that safe is not the deferral itself but the rule
 * that comes with it: while an update is pending, the in-core copy -- not the
 * device -- is what every reader gets.  Without that, open() reads the size
 * off the device and a file closed and reopened reports the length it had at
 * whichever write last happened to store it, with reads stopping there.
 *
 * That rule is enforced in one place.  Both routes to an inode
 * (ext4_read_inode_loc, and ext4_get_inode_cached in front of it) apply the
 * pending change to what they hand back, so there is no such thing as a
 * reader that sees the stale value -- stat, open, the writeback path and
 * every future caller included.
 *
 * Only size and timestamps are ever deferred.  A write that ALLOCATES stores
 * the inode immediately, because the block map lives in the inode too, and a
 * deferred copy of that would leave the filesystem not knowing it owns the
 * blocks the data went into.
 *
 * Fixed size, and deliberately so: no allocation, therefore usable from
 * anywhere, and a program writing to more files at once than this holds does
 * not fail -- it falls back to storing immediately, which is what the driver
 * did for all of them before.
 */
/* A leaf lock: nothing is taken under it and nothing under it sleeps, so it is
 * safe from any context that can reach an inode read.  Guarding the table this
 * way rather than relying on the metadata lock means a reader that reaches an
 * inode by some path not yet audited still sees the pending value. */

/* Record a pending size/time update.  Returns 0 if there was no room, in which
 * case the caller must store the inode itself. */
static int ext4_di_record(ext4_fs_t *fs, unsigned long ino, uint64_t size, uint32_t mtime,
			  uint32_t ctime)
{
	uint64_t flags;
	int done = 0;

	if (ino == 0)
		return 0;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		if (fs->st->di[i].ino == ino) {
			fs->st->di[i].size = size;
			fs->st->di[i].mtime = mtime;
			fs->st->di[i].ctime = ctime;
			done = 1;
			break;
		}
	}
	if (!done && fs->st->di_n < EXT4_DI_MAX) {
		fs->st->di[fs->st->di_n].ino = ino;
		fs->st->di[fs->st->di_n].size = size;
		fs->st->di[fs->st->di_n].mtime = mtime;
		fs->st->di[fs->st->di_n].ctime = ctime;
		fs->st->di_n++;
		done = 1;
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
	return done;
}

/* Forget the pending update for `ino`: it has just been stored, or the inode
 * it described no longer exists. */
static void ext4_di_clear(ext4_fs_t *fs, unsigned long ino)
{
	uint64_t flags;

	if (ino == 0)
		return;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		if (fs->st->di[i].ino == ino) {
			fs->st->di[i] = fs->st->di[fs->st->di_n - 1];
			fs->st->di[fs->st->di_n - 1].ino = 0;
			fs->st->di_n--;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
}

/* Is there an update pending for this inode? */
static int ext4_di_pending(ext4_fs_t *fs, unsigned long ino)
{
	uint64_t flags;
	int found = 0;

	if (ino == 0)
		return 0;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++)
		if (fs->st->di[i].ino == ino) {
			found = 1;
			break;
		}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
	return found;
}

/* Overlay the pending update onto a freshly-read inode.
 *
 * Idempotent, so applying it twice on the way through the parsed-inode cache
 * costs nothing and cannot get it wrong.
 */
static void ext4_di_apply(ext4_fs_t *fs, unsigned long ino, ext4_inode *in)
{
	uint64_t flags;

	if (!in || ino == 0)
		return;
	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		if (fs->st->di[i].ino != ino)
			continue;
		in->i_size_lo = (uint32_t)fs->st->di[i].size;
		if ((in->i_mode & S_IFMT) == S_IFREG)
			in->i_size_high = (uint32_t)(fs->st->di[i].size >> 32);
		ext4_stamp(fs, in, EXT4_T_MTIME, (int64_t)fs->st->di[i].mtime,
			   0);
		ext4_stamp(fs, in, EXT4_T_CTIME, (int64_t)fs->st->di[i].ctime,
			   0);
		break;
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
}

static int ext4_read_inode_loc(ext4_fs_t *fs, unsigned long ino,
			       ext4_inode *out, unsigned long *out_block,
			       unsigned *out_off)
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
	unsigned long blk = itbl + byte / fs->block_size;
	unsigned off = byte % fs->block_size;

	uint8_t *buf = (uint8_t *)ext4_bget(fs);
	if (!buf)
		return ST_NOMEM;
	st = ext4_read_block(fs, blk, buf);
	if (st != ST_OK) {
		ext4_bput(fs, buf);
		return st;
	}

	/* Verify the inode's metadata_csum using the full on-disk
     * bytes still in `buf` (the struct copy below truncates them).  A mismatch
     * means the inode is corrupt — refuse to hand it back (ST_IO => -EIO) and
     * mark the filesystem errored, exactly like the reference's -EFSBADCRC. */
	if (fs->has_metadata_csum) {
		const uint8_t *rp = buf + off;
		uint32_t got = ext4_inode_csum(fs, ino, rp);
		uint16_t slo;
		mm_memcpy(&slo, rp + EXT4_INO_CSUM_LO_OFF, 2);
		uint32_t want = slo, mask = 0xFFFFu;
		if (fs->inode_size > EXT4_GOOD_OLD_ISIZE) {
			uint16_t extra;
			mm_memcpy(&extra,
				  rp + __builtin_offsetof(ext4_inode,
							  i_extra_isize),
				  2);
			if (extra >=
			    (EXT4_INO_CSUM_HI_OFF + 2) - EXT4_GOOD_OLD_ISIZE) {
				uint16_t shi;
				mm_memcpy(&shi, rp + EXT4_INO_CSUM_HI_OFF, 2);
				want |= (uint32_t)shi << 16;
				mask = 0xFFFFFFFFu;
			}
		}
		if ((got & mask) != want) {
			kprintf("ext4: inode %lu metadata_csum mismatch "
				"(disk 0x%x computed 0x%x)\n",
				ino, want, got & mask);
			ext4_bput(fs, buf);
			ext4_fs_error(fs, "inode checksum mismatch", ino);
			return ST_IO;
		}
	}

	mm_memset(out, 0, sizeof(*out));
	unsigned copy =
		fs->inode_size < sizeof(*out) ? fs->inode_size : sizeof(*out);
	mm_memcpy(out, buf + off, copy);
	ext4_bput(fs, buf);

	/* What is on the device is not necessarily what the file is.  If a
	 * write has changed the size or the timestamps and that change has not
	 * been stored yet, this is where it is put back -- every reader of an
	 * inode arrives here or at the cache in front of it, so there is no
	 * route by which the stale value escapes. */
	ext4_di_apply(fs, ino, out);

	if (out_block)
		*out_block = blk;
	if (out_off)
		*out_off = off;
	return ST_OK;
}

/* Small parsed-inode cache.  Was a SINGLE entry, which path resolution
 * thrashed on every component (directory inode, then child inode, then the
 * next directory...), forcing an inode-table block read + inode checksum
 * verification per step.  A handful of entries covers a whole resolve.
 *
 * Copy-out API: with concurrent shared-mode readers, a pointer into the
 * cache array would be a use-after-recycle hazard (another reader's fill
 * can reuse the slot at any time).  Lookups therefore copy the record into
 * a caller-owned buffer under the spinlock; the disk fill on a miss runs
 * unlocked (a racing double-fill of the same inode is benign). */

static void ext4_inode_cache_flush(ext4_fs_t *fs)
{
	uint64_t flags;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	for (int i = 0; i < EXT4_IC_ENTRIES; i++)
		fs->st->ic[i].ino = 0;
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
}

/* Fetch inode `ino` into caller-owned `out`.  Returns 1 on success, 0 on
 * failure. */
static int ext4_get_inode_cached(ext4_fs_t *fs, unsigned long ino,
				 ext4_inode *out)
{
	if (ino == 0 || !out)
		return 0;
	uint64_t flags;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	for (int i = 0; i < EXT4_IC_ENTRIES; i++)
		if (fs->st->ic[i].ino == ino) {
			mm_memcpy(out, &fs->st->ic[i].inode, sizeof(*out));
			spin_unlock_irqrestore(&fs->st->ic_lock, flags);
			/* The same rule as on the device path below: a pending
			 * size or timestamp is what the file actually has.
			 * This copy is normally already current -- the write
			 * path updates it in place -- but it is also whatever
			 * some earlier reader left here, so it is overlaid
			 * rather than trusted.  Doing it twice is harmless. */
			ext4_di_apply(fs, ino, out);
			return 1;
		}
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
	/* Miss: read from disk without the spinlock (sleeps on I/O). */
	if (ext4_read_inode_loc(fs, ino, out, 0, 0) != ST_OK)
		return 0;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	unsigned slot = fs->st->ic_next++ % EXT4_IC_ENTRIES;
	mm_memcpy(&fs->st->ic[slot].inode, out, sizeof(*out));
	fs->st->ic[slot].ino = ino;
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
	return 1;
}

/*
 * Replace the cached copy of one inode with a newer one.
 *
 * The alternative, and what the write path used to do, is to empty the whole
 * cache whenever any inode changes.  That is correct but throws away seven
 * other inodes to update one, and the very next thing a write does is map a
 * block through the inode it just changed -- so the entry it needs is read back
 * off the device, once per write() call.
 *
 * Updating in place keeps the map lookups that follow a write in memory, and
 * leaves the other entries alone.  Inserts if the inode was not cached: the
 * caller has the freshest copy there is, so it is worth keeping.
 */
static void ext4_inode_cache_update(ext4_fs_t *fs, unsigned long ino, const ext4_inode *in)
{
	uint64_t flags;

	if (ino == 0 || !in)
		return;
	spin_lock_irqsave(&fs->st->ic_lock, &flags);
	for (int i = 0; i < EXT4_IC_ENTRIES; i++) {
		if (fs->st->ic[i].ino == ino) {
			mm_memcpy(&fs->st->ic[i].inode, in, sizeof(fs->st->ic[i].inode));
			spin_unlock_irqrestore(&fs->st->ic_lock, flags);
			return;
		}
	}
	{
		unsigned slot = fs->st->ic_next++ % EXT4_IC_ENTRIES;

		mm_memcpy(&fs->st->ic[slot].inode, in, sizeof(fs->st->ic[slot].inode));
		fs->st->ic[slot].ino = ino;
	}
	spin_unlock_irqrestore(&fs->st->ic_lock, flags);
}

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

/* ===================================================================
 * Logical -> physical block mapping (extents + classic indirect)
 * =================================================================== */

/* Walk an extent tree to map logical block `lidx` -> physical block.
 * Returns 0 for a hole / past-EOF / error. */
static unsigned long ext4_extent_map(ext4_fs_t *fs, unsigned long ino,
				     const ext4_inode *in, unsigned long lidx)
{
	const uint8_t *node = in->i_block; /* root lives inline in the inode */
	uint8_t *heap = 0;
	unsigned long result = 0;

	for (int depth_guard = 0; depth_guard < 6; depth_guard++) {
		const ext4_extent_header *eh = (const ext4_extent_header *)node;
		if (eh->eh_magic != EXT4_EXT_MAGIC) {
			WARN_ON_ONCE(1); /* corrupt extent header magic */
			break;
		}
		unsigned entries = eh->eh_entries;
		if (eh->eh_depth == 0) {
			const ext4_extent *ex = (const ext4_extent *)(eh + 1);
			for (unsigned i = 0; i < entries; i++) {
				unsigned long start = ex[i].ee_block;
				unsigned len = ex[i].ee_len;
				if (len > 32768)
					len -= 32768; /* uninitialized extent */
				if (lidx >= start && lidx < start + len) {
					unsigned long phys =
						(unsigned long)ex[i]
							.ee_start_lo |
						((unsigned long)ex[i]
							 .ee_start_hi
						 << 32);
					result = phys + (lidx - start);
					goto done;
				}
			}
			break; /* no covering extent: hole / EOF */
		} else {
			const ext4_extent_idx *ix =
				(const ext4_extent_idx *)(eh + 1);
			int chosen = -1;
			for (unsigned i = 0; i < entries; i++) {
				if (ix[i].ei_block <= lidx)
					chosen = (int)i;
				else
					break;
			}
			if (chosen < 0)
				break;
			unsigned long child =
				(unsigned long)ix[chosen].ei_leaf_lo |
				((unsigned long)ix[chosen].ei_leaf_hi << 32);
			if (!heap) {
				heap = (uint8_t *)ext4_bget(fs);
				if (!heap)
					break;
			}
			if (ext4_read_block(fs, child, heap) != ST_OK)
				break;
			if (!ext4_mbc_verified(fs, child)) {
				if (!ext4_extent_block_csum_ok(
					    fs, ino, in->i_generation, heap)) {
					ext4_fs_error(
						fs,
						"extent block checksum mismatch",
						ino);
					break; /* corrupt index/leaf node: treat as unmapped */
				}
				ext4_mbc_mark_verified(fs, child);
			}
			node = heap; /* descend */
		}
	}
done:
	if (heap)
		ext4_bput(fs, heap);
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
	uint32_t *buf = (uint32_t *)ext4_bget(fs);
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
			unsigned long rem = lidx % (ppb * ppb);
			if (dind && ext4_read_block(fs, dind, buf) == ST_OK) {
				unsigned long ind = buf[rem / ppb];
				if (ind &&
				    ext4_read_block(fs, ind, buf) == ST_OK)
					result = buf[rem % ppb];
			}
		}
	}
out:
	ext4_bput(fs, buf);
	return result;
}

/* Map (inode, logical block index) -> physical block number (0 = hole/EOF). */
static unsigned long ext4_block_map(ext4_fs_t *fs, unsigned long ino,
				    unsigned long lidx)
{
	ext4_inode in;
	if (!ext4_get_inode_cached(fs, ino, &in))
		return 0;
	if (in.i_flags & EXT4_INODE_EXTENTS_FL)
		return ext4_extent_map(fs, ino, &in, lidx);
	return ext4_indirect_map(fs, &in, lidx);
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
{
	return ((ext4_fs_t *)sb->fs_private)->block_size;
}

static const block_device_t *ext4_sb_bdev(vfs_superblock_t *sb)
{
	return ((ext4_fs_t *)sb->fs_private)->bdev;
}

static unsigned long ext4_sb_sector_size(vfs_superblock_t *sb)
{
	return ((ext4_fs_t *)sb->fs_private)->bdev->sector_size;
}

static unsigned long ext4_sb_block_to_lba(vfs_superblock_t *sb,
					  unsigned long bid)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	if (!EXT4_BID_IS(bid))
		return 0;
	unsigned long pbn =
		ext4_block_map(fs, EXT4_BID_INO(bid), EXT4_BID_LIDX(bid));
	if (pbn == EXT4_HOLE_PBN)
		return 0; /* hole: pagecache zero-fills */
	return fs->part_lba_offset + pbn * fs->sectors_per_block;
}

static unsigned long ext4_sb_end_of_chain(vfs_superblock_t *sb)
{
	(void)sb;
	return EXT4_EOC_MARKER;
}

static unsigned long ext4_sb_next_block(vfs_superblock_t *sb, unsigned long bid)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	if (!EXT4_BID_IS(bid))
		return EXT4_BID_EOC;
	unsigned long ino = EXT4_BID_INO(bid);
	unsigned long lidx = EXT4_BID_LIDX(bid);
	/* Advance to the next logical block, ending the chain only at the real
	 * end of file — NOT at the first hole.  A sparse file (e.g. a linked
	 * binary with a page-aligned run of zeros, which mkfs stores as a hole)
	 * has unmapped blocks in the MIDDLE; the old "0 mapping means EOF" test
	 * truncated the chain there, so every block past the hole became
	 * unreadable and exec of such a binary failed with ENOEXEC.  The block
	 * count comes from the inode size; the read path zero-fills any block
	 * whose physical mapping is a hole (ext4_sb_block_to_lba returns 0). */
	ext4_inode in;
	if (!ext4_get_inode_cached(fs, ino, &in))
		return EXT4_BID_EOC;
	unsigned long size = ext4_inode_size(&in);
	unsigned long bs = fs->block_size ? fs->block_size : 4096;
	unsigned long total_blocks = (size + bs - 1) / bs;
	if (lidx + 1 >= total_blocks)
		return EXT4_BID_EOC;
	return EXT4_BID_ENC(ino, lidx + 1);
}

/* Block N of a chain, without walking (vfs_sb_ops_t.block_at): the same
 * answer as N calls of ext4_sb_next_block -- the positional id while N lies
 * inside the recorded size, holes included, 0 past it. */
static unsigned long ext4_sb_block_at(vfs_superblock_t *sb,
				      unsigned long chain_id,
				      unsigned long block_index)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	ext4_inode in;

	if (!fs || !EXT4_BID_IS(chain_id) || block_index > 0xFFFFFFFFUL)
		return 0;
	unsigned long ino = EXT4_BID_INO(chain_id);
	if (ino == 0 || ino > fs->inodes_count ||
	    !ext4_get_inode_cached(fs, ino, &in))
		return 0;
	unsigned long bs = fs->block_size ? fs->block_size : 4096;
	unsigned long total = (ext4_inode_size(&in) + bs - 1) / bs;
	if (block_index >= total)
		return 0;
	return EXT4_BID_ENC(ino, block_index);
}

/* Map block N of a file directly.  ext4 block ids are positional -- block N of
 * inode I is EXT4_BID_ENC(I, N) -- so there is nothing to walk; the only
 * question is whether the block exists, and the extent tree answers it.
 *
 * This is deliberately NOT the size test that ext4_sb_next_block ends a chain
 * on.  The size is a second opinion about how much of the file is real, and
 * when it disagrees with the extent tree the extent tree is right: the write
 * path accepts a page for caching only after ext4_block_map says the block is
 * there, so a page that exists and is dirty has an extent behind it whatever
 * the recorded length happens to say at the moment writeback asks. */
static unsigned long ext4_sb_map_block(vfs_superblock_t *sb,
				       unsigned long chain_id,
				       unsigned long block_index)
{
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	if (!fs || !EXT4_BID_IS(chain_id))
		return 0;
	unsigned long ino = EXT4_BID_INO(chain_id);
	if (ino == 0 || ino > fs->inodes_count)
		return 0;
	/* A hole reads as zeroes and has no block to store into; block_to_lba
	 * makes the same call on the same value. */
	if (ext4_block_map(fs, ino, block_index) == 0)
		return 0;
	return EXT4_BID_ENC(ino, block_index);
}

static void ext4_sb_lock_io(vfs_superblock_t *sb)
{
	ext4_io_lock((ext4_fs_t *)sb->fs_private);
}
static void ext4_sb_unlock_io(vfs_superblock_t *sb)
{
	ext4_io_unlock((ext4_fs_t *)sb->fs_private);
}

/* Shared mapping lock for the pagecache: extent lookups (next_block /
 * block_to_lba) only read metadata, so concurrent readers may map in
 * parallel; mutators hold the metadata lock exclusive.  The pagecache
 * performs the data transfer with no fs lock — data-block lifetime is
 * fenced by the per-inode lock the read entry point holds. */
static void ext4_sb_lock_map(vfs_superblock_t *sb)
{
	ext4_meta_rlock((ext4_fs_t *)sb->fs_private);
}
static void ext4_sb_unlock_map(vfs_superblock_t *sb)
{
	ext4_meta_runlock((ext4_fs_t *)sb->fs_private);
}

static unsigned long ext4_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	(void)sb;
	return 0;
}

/* Persist a dirty cached inode's size back to disk (called by icache_flush).
 * Defined out-of-line below where the write helpers are available. */
static int ext4_write_inode_struct(ext4_fs_t *fs, unsigned long ino,
				   const ext4_inode *in);
static int ext4_read_inode_loc(ext4_fs_t *fs, unsigned long ino,
			       ext4_inode *out, unsigned long *out_block,
			       unsigned *out_off);

static int ext4_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *inode)
{
	if (!inode)
		return 0;
	ext4_fs_t *fs = (ext4_fs_t *)sb->fs_private;
	unsigned long ino = EXT4_BID_INO(inode->start_cluster);
	if (ino == 0 || ino > fs->inodes_count)
		return 0;
	/* This is reached from the generic sync path (pagecache_sync ->
	 * icache_flush_all) with NO ext4 lock held, yet it does a read-modify-
	 * write of an inode-table block THROUGH the journal/mbc tiers
	 * (ext4_read_block -> st->txn/st->ckpt/mbc, ext4_write_block -> st->txn
	 * capture).  Every other st->txn/mbc mutator holds ext4_io_lock (the
	 * invariant documented at ext4_read_block / the st->txn comment); this one
	 * did not.  Unlocked, it races a concurrent mkdir/dir-grow mutating
	 * st->txn: it can corrupt the st->txn array (concurrent append/realloc) or
	 * write back a whole inode block whose OTHER inode slots (e.g. a
	 * directory sharing the 4 KB block) still hold pre-grow values —
	 * reverting that directory's i_size.  Since ext4_dir_lookup linearly
	 * scans blocks 0..i_size/blocksize, a reverted (smaller) i_size makes it
	 * miss the just-added entry in the grown tail leaf -> spurious ENOENT,
	 * only ever on multi-block (large/htree) directories.  Take the
	 * (reentrant) io lock so the RMW is serialized and reads the current
	 * block.  Safe nested: same owner just bumps wdepth. */
	ext4_io_lock(fs);
	ext4_inode in;
	int rc = -1;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) == ST_OK) {
		/* INVARIANT: an ext4 directory's i_size only ever grows — entries
		 * are tombstoned (rec_len merge), never removed, and dir blocks are
		 * appended, never truncated.  Writing back a SMALLER i_size for a
		 * directory reverts it, which is the corruption signature: a stale
		 * inode-block RMW clobbered a concurrent dir-grow.  It shrinks the
		 * block range ext4_dir_lookup linearly scans, so it misses the
		 * just-added entry in the grown tail leaf (spurious ENOENT on
		 * large/htree dirs).  Catch the revert at the moment it is written. */
		WARN_RATELIMIT((in.i_mode & S_IFMT) == S_IFDIR &&
				       (uint32_t)inode->size < in.i_size_lo,
			       "ext4: dir ino %lu i_size reverting %u -> %u (stale inode-block RMW / corruption)",
			       ino, in.i_size_lo, (uint32_t)inode->size);
		/* Take the size from the generic cache only when the write path
		 * has nothing pending for this inode.
		 *
		 * When it does, the read above has already put that size in,
		 * and it is the better of the two: it is written by the code
		 * that computed the length, while this one is a copy kept per
		 * open handle, which two handles on the same file can move
		 * backwards between them.  Preferring it here would persist
		 * the shorter length AND discard the pending record in the
		 * same step, so the difference would not be recoverable. */
		if (!ext4_di_pending(fs, ino)) {
			/* ... and even then, only FORWARDS.
			 *
			 * ext4_di_pending(fs) is not a reliable guard on its own:
			 * ext4_di_record(fs) fails when its table is full, and the
			 * write path then stores the inode instead and leaves
			 * nothing pending -- so a growing file can arrive here
			 * with the generic copy still holding a length from
			 * before the write.  Persisting it shrinks the file to
			 * something it stopped being.
			 *
			 * The read above is the better value in that case: it
			 * comes from the inode with any pending record already
			 * applied.  A shrink that is real -- truncate -- gets to
			 * the device through ext4_truncate_impl, which stores the
			 * inode itself and clears the pending record, so the
			 * value read here is ALREADY the short one and there is
			 * nothing for this path to shorten.  Every remaining
			 * backwards step is therefore a stale copy, and taking it
			 * costs the tail of the file: the size is what
			 * ext4_sb_next_block derives the block chain's length
			 * from, so the pages past the reverted end can no longer
			 * be mapped, and writeback drops them with "dirty page
			 * ... has no block behind it". */
			uint64_t on_inode = ext4_inode_size(&in);
			if ((uint64_t)inode->size >= on_inode) {
				in.i_size_lo = (uint32_t)inode->size;
				if ((in.i_mode & S_IFMT) == S_IFREG)
					in.i_size_high =
						(uint32_t)(inode->size >> 32);
			} else {
				WARN_RATELIMIT(
					1,
					"ext4: ino %lu size revert refused %lu -> %lu (stale generic-cache copy)",
					ino, (unsigned long)on_inode,
					(unsigned long)inode->size);
			}
		}
		ext4_inode_cache_flush(fs);
		ext4_write_inode_struct(fs, ino, &in);
		rc = 0;
	}
	ext4_io_unlock(fs);
	return rc;
}

static const vfs_sb_ops_t ext4_sb_ops = {
	.block_size = ext4_sb_block_size,
	.bdev = ext4_sb_bdev,
	.sector_size = ext4_sb_sector_size,
	.block_to_lba = ext4_sb_block_to_lba,
	.end_of_chain_marker = ext4_sb_end_of_chain,
	.next_block = ext4_sb_next_block,
	.map_block = ext4_sb_map_block,
	.block_at = ext4_sb_block_at,
	.write_inode = ext4_sb_write_inode,
	.lock_io = ext4_sb_lock_io,
	.unlock_io = ext4_sb_unlock_io,
	.lock_map = ext4_sb_lock_map,
	.unlock_map = ext4_sb_unlock_map,
	.reserved_meta_block = ext4_sb_reserved_meta_block,
};

static void ext4_sb_attach(ext4_fs_t *fs)
{
	fs->sb.ops = &ext4_sb_ops;
	fs->sb.fs_private = fs;
}

/* ===================================================================
 * Directory traversal + path resolution
 * =================================================================== */

/* ---- Dentry cache -------------------------------------------------------
 *
 * Every path resolution used to read the directory off the device, component
 * by component, on every open, stat, access and unlink.  A program that opens
 * a few thousand files pays for a few thousand directory scans, and a $PATH
 * search pays for one per directory that does NOT hold the program.
 *
 * So the answers are remembered: name -> inode, keyed by the directory the
 * name is in.  Failures are remembered too -- a negative entry -- which is
 * what makes the $PATH case cheap, because "not here" is the answer that
 * dominates it.
 *
 * Two properties of this cache are worth stating rather than leaving to be
 * rediscovered:
 *
 *  - It is maintained, not invalidated.  A create adds the entry it just
 *    made, a delete removes exactly the one it removed.  Both happen in
 *    ext4_dir_add and ext4_dir_del, which is where EVERY directory mutation
 *    in this driver goes through -- create, unlink, mkdir, rmdir, rename,
 *    symlink and link all reach the device that way, so none of them can be
 *    forgotten here by being added later.
 *
 *  - Nothing needs a lock of its own.  Lookups run under the metadata lock
 *    SHARED and mutations under it EXCLUSIVE, so a mutation cannot interleave
 *    with a lookup; the cache's internal locking then handles two concurrent
 *    lookups.  Entries are made only AFTER the device operation has
 *    succeeded, so a failed mutation leaves nothing behind.
 *
 * The key is the inode number in the same encoded form the page and inode
 * caches use.  Encoding it keeps ext4's keys out of the range FAT32's cluster
 * numbers occupy: the caches are shared, and two filesystems must not be able
 * to name the same entry.
 */
#define EXT4_DC_PARENT(ino) EXT4_BID_ENC((ino), 0)

/* Longest name an ext4 directory entry can hold (name_len is one byte). */
#define EXT4_NAME_LEN 255

/* The lookup interface takes a NUL-terminated name; this driver passes names
 * as a pointer plus a length, which is not always the same thing.  Copying is
 * a few dozen bytes against the directory scan it avoids. */
static int ext4_dc_name(const char *name, unsigned name_len, char *out,
			unsigned cap)
{
	if (name_len == 0 || name_len >= cap)
		return 0;
	mm_memcpy(out, name, name_len);
	out[name_len] = '\0';
	return 1;
}

/* Consult the cache.  Returns ST_OK (with *out_ino / *out_ftype filled in),
 * ST_NOT_FOUND for a remembered failure, or ST_UNSUPPORTED for "not cached,
 * ask the device" -- a value no directory lookup produces, so a caller cannot
 * confuse it with a real answer. */
static int ext4_dc_lookup(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			  unsigned name_len, unsigned long *out_ino,
			  unsigned *out_ftype)
{
	char nm[EXT4_NAME_LEN + 1];
	dc_result_t res;

	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return ST_UNSUPPORTED;

	switch (dcache_lookup_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm, &res)) {
	case DC_LOOKUP_FOUND:
		if (out_ino)
			*out_ino = res.ino;
		if (out_ftype)
			*out_ftype = res.attr;
		return ST_OK;
	case DC_LOOKUP_NEGATIVE:
		return ST_NOT_FOUND;
	default:
		return ST_UNSUPPORTED;
	}
}

static void ext4_dc_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long child_ino,
			unsigned ftype)
{
	char nm[EXT4_NAME_LEN + 1];

	if (child_ino == 0)
		return;
	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return;
	/* The file type is stored as ext4 records it in the directory entry.
	 * The cache's own directory hint (bit 0x10) is left unset: nothing
	 * here reads it, because ext4 takes what it needs from the inode.
	 *
	 * The size is deliberately NOT stored, hence the zero.  ext4 keeps a
	 * file's size in its inode and nowhere else, so caching a copy of it
	 * against a NAME would mean every write had to find and update that
	 * copy -- and a missed update is a file that stats short and reads
	 * truncated.  A name resolves to an inode here; the inode answers for
	 * everything else. */
	dcache_insert_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm, child_ino, 0, ftype);
}

static void ext4_dc_add_negative(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
				 unsigned name_len)
{
	char nm[EXT4_NAME_LEN + 1];

	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return;
	dcache_insert_negative_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm);
}

static void ext4_dc_del(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len)
{
	char nm[EXT4_NAME_LEN + 1];

	if (!ext4_dc_name(name, name_len, nm, sizeof(nm)))
		return;
	/* Removed rather than turned into a negative entry.  Both are correct,
	 * and removing is the one that stays correct if a future caller ever
	 * puts the name back without going through ext4_dir_add. */
	dcache_invalidate_cs(&fs->sb, EXT4_DC_PARENT(dir_ino), nm);
}

/* Forget everything cached under `ino` as a directory.
 *
 * Needed at the two points where an inode number stops meaning what it meant:
 * when a directory is removed, and when a number is handed out again. */
static void ext4_dc_drop_dir(ext4_fs_t *fs, unsigned long ino)
{
	if (ino == 0)
		return;
	dcache_invalidate_dir(&fs->sb, EXT4_DC_PARENT(ino));
}

/* Look up `name` (length name_len) in directory inode `dir_ino`, on the device.
 * On success returns ST_OK and sets *out_ino (and *out_ftype if non-NULL).
 *
 * *out_complete says whether every block of the directory was actually read.
 * A "not found" from a scan that skipped a block it could not read is not the
 * same statement as a "not found" from a scan that read all of them: the first
 * may be a transient device error, and remembering it would make that error
 * permanent for as long as the entry survives. */
static int ext4_dir_lookup_disk(ext4_fs_t *fs, unsigned long dir_ino,
				const char *name, unsigned name_len,
				unsigned long *out_ino, unsigned *out_ftype,
				int *out_complete)
{
	ext4_inode din;
	int complete = 1;

	if (out_complete)
		*out_complete = 0;
	if (!ext4_get_inode_cached(fs, dir_ino, &din) ||
	    (din.i_mode & S_IFMT) != S_IFDIR)
		return ST_NOT_FOUND;
	unsigned long dsize = ext4_inode_size(&din);
	unsigned long nblocks = (dsize + fs->block_size - 1) / fs->block_size;
	uint32_t gen = din.i_generation; /* for the leaf csum */

	uint8_t *blk = (uint8_t *)ext4_bget(fs);
	if (!blk)
		return ST_NOMEM;

	for (unsigned long b = 0; b < nblocks; b++) {
		unsigned long pbn = ext4_block_map(fs, dir_ino, b);
		if (pbn == 0)
			continue; /* sparse dir block (rare): genuinely empty */
		if (ext4_read_block(fs, pbn, blk) != ST_OK) {
			complete = 0;
			continue;
		}
		if (!ext4_mbc_verified(fs, pbn)) {
			if (!ext4_dir_csum_ok(fs, dir_ino, gen, blk)) {
				ext4_bput(fs, blk);
				ext4_fs_error(fs,
					      "directory leaf checksum mismatch",
					      dir_ino);
				return ST_IO;
			}
			ext4_mbc_mark_verified(fs, pbn);
		}
		unsigned off = 0;
		while (off + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > fs->block_size) {
				WARN_ON_ONCE(
					1); /* bad dir rec_len — stop scanning this block */
				complete = 0;
				break;
			}
			if (de->inode != 0 && de->name_len == name_len &&
			    ext4_memcmp(de->name, name, name_len) == 0) {
				*out_ino = de->inode;
				if (out_ftype)
					*out_ftype = de->file_type;
				ext4_bput(fs, blk);
				if (out_complete)
					*out_complete = 1;
				return ST_OK;
			}
			off += rec;
		}
	}
	ext4_bput(fs, blk);
	if (out_complete)
		*out_complete = complete;
	return ST_NOT_FOUND;
}

/*
 * Look up `name` in directory `dir_ino`, through the dentry cache.
 *
 * A hit answers without touching the device at all.  A miss does the scan
 * above and then records what it found, so the next caller does not repeat it.
 *
 * "." is not cached: path resolution never asks for it (it means the directory
 * that was already reached), so an entry for it would only take up room.
 */
static int ext4_dir_lookup(ext4_fs_t *fs, unsigned long dir_ino,
			   const char *name, unsigned name_len,
			   unsigned long *out_ino, unsigned *out_ftype)
{
	unsigned long ino = 0;
	unsigned ftype = 0;
	int complete = 0;
	int r;

	if (!out_ino)
		return ST_INVALID;

	r = ext4_dc_lookup(fs, dir_ino, name, name_len, &ino, &ftype);
	if (r != ST_UNSUPPORTED) {
		if (r == ST_OK) {
			*out_ino = ino;
			if (out_ftype)
				*out_ftype = ftype;
		}
		return r;
	}

	r = ext4_dir_lookup_disk(fs, dir_ino, name, name_len, &ino, &ftype,
				 &complete);
	if (r == ST_OK) {
		*out_ino = ino;
		if (out_ftype)
			*out_ftype = ftype;
		ext4_dc_add(fs, dir_ino, name, name_len, ino, ftype);
	} else if (r == ST_NOT_FOUND && complete) {
		/* Remembered only when the whole directory was read.  This is
		 * the entry that makes searching a $PATH cheap: most of the
		 * directories on it do not hold the program, and without it
		 * each of them is rescanned on every command. */
		ext4_dc_add_negative(fs, dir_ino, name, name_len);
	}
	return r;
}

/* Read a symlink's target into buf (NUL-terminated).  Fast symlinks (<60B,
 * i_blocks==0) store the target inline in i_block; longer "slow" symlinks
 * store it in data block 0.  Returns target length, or -1 on error. */
static int ext4_read_symlink_target(ext4_fs_t *fs, unsigned long ino,
				    const ext4_inode *in, char *buf,
				    unsigned cap)
{
	unsigned long len = in->i_size_lo;
	if (len == 0 || len >= cap) {
		if (len >= cap)
			len = cap - 1;
		else
			return -1;
	}
	if (in->i_blocks_lo == 0) {
		mm_memcpy(buf, in->i_block, len);
	} else {
		unsigned long pbn = ext4_block_map(fs, ino, 0);
		if (pbn == 0)
			return -1;
		uint8_t *blk = (uint8_t *)ext4_bget(fs);
		if (!blk)
			return -1;
		if (ext4_read_sectors(fs->bdev,
				      fs->part_lba_offset +
					      pbn * fs->sectors_per_block,
				      fs->sectors_per_block, blk) != ST_OK) {
			ext4_bput(fs, blk);
			return -1;
		}
		mm_memcpy(buf, blk, len);
		ext4_bput(fs, blk);
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
	if (!path || !out_ino)
		return ST_INVALID;
	if (depth > 12)
		return ST_INVALID; /* ELOOP guard            */

	/* A trailing slash names the thing THROUGH the link, and it has to be
	 * a directory.
	 *
	 * "a/b/" means the same as "a/b/.", so the last component is followed
	 * even by the callers that otherwise stop at a link -- lstat and
	 * friends -- and the answer must be a directory or the path is wrong.
	 * Without this, listing a directory reached by a symbolic link
	 * reported the link itself however the name was written, because the
	 * tool asks with lstat and a trailing slash is exactly how one says
	 * "the directory, not the link". */
	size_t plen = 0;
	while (path[plen])
		plen++;
	int trailing_slash = (plen > 0 && path[plen - 1] == '/');
	if (trailing_slash)
		follow_final = 1;

	unsigned long cur = (path[0] == '/') ? EXT4_ROOT_INO : start_ino;
	const char *p = path;
	while (*p == '/')
		p++;

	char comp[256];
	while (*p) {
		unsigned ci = 0;
		const char *cstart = p;
		while (*p && *p != '/') {
			if (ci < sizeof(comp) - 1)
				comp[ci++] = *p;
			p++;
		}
		comp[ci] = '\0';
		const char *cend = p;
		while (*p == '/')
			p++;
		int is_last = (*p == '\0');

		if (ci == 0 || (ci == 1 && comp[0] == '.'))
			continue;
		if (ci == 2 && comp[0] == '.' && comp[1] == '.') {
			unsigned long parent;
			if (ext4_dir_lookup(fs, cur, "..", 2, &parent, 0) ==
			    ST_OK)
				cur = parent;
			continue;
		}
		unsigned long child;
		unsigned ftype = 0;
		int lr = ext4_dir_lookup(fs, cur, comp, ci, &child, &ftype);
		if (lr != ST_OK) /* a corrupt directory leaf
                                                     returns ST_IO; propagate it
                                                     rather than masking ENOENT  */
			return lr;

		ext4_inode cin;
		if (!ext4_get_inode_cached(fs, child, &cin))
			return ST_IO;
		if ((cin.i_mode & S_IFMT) == S_IFLNK &&
		    (!is_last || follow_final)) {
			char target[256];
			int tl = ext4_read_symlink_target(
				fs, child, &cin, target, sizeof(target));
			if (tl <= 0)
				return ST_IO;
			/* The path the VFS handed us: the VFS follows the
			 * link, wherever in the name space it leads (another
			 * mount, the process's root, ".." past ours). */
			if (depth == 0 && path[0] == '/' &&
			    vfs_link_hop(fs->mnt, path,
					 (unsigned long)(cstart - path),
					 (unsigned long)(cend - path), target))
				return ST_LINK;
			unsigned long linked;
			int r = ext4_resolve_ex(fs, cur, target, 1, &linked,
						depth + 1);
			if (r != ST_OK)
				return r;
			cur = linked;
		} else {
			cur = child;
		}
		if (!is_last) {
			ext4_inode d;
			if (!ext4_get_inode_cached(fs, cur, &d))
				return ST_IO; /* unreadable/corrupt component */
			if ((d.i_mode & S_IFMT) != S_IFDIR)
				return ST_NOT_FOUND;
		}
	}
	if (trailing_slash) {
		ext4_inode d;
		if (!ext4_get_inode_cached(fs, cur, &d))
			return ST_IO;
		if ((d.i_mode & S_IFMT) != S_IFDIR)
			return ST_NOT_FOUND; /* "file/" is not a path */
	}
	*out_ino = cur;
	return ST_OK;
}

/* Default resolver: relative to cwd, follows a final symlink. */
static int ext4_resolve(ext4_fs_t *fs, const char *path, unsigned long *out_ino)
{
	return ext4_resolve_ex(fs, fs->st->cwd_ino, path, 1, out_ino, 0);
}

/* ===================================================================
 * VFS operations
 * =================================================================== */

static const vfs_ops_t ext4_vfs_ops; /* forward */

/* Write helpers (defined further below; used by open/truncate). */
static int ext4_resolve_parent(ext4_fs_t *fs, const char *path,
			       unsigned long *parent_ino, char *name_out,
			       unsigned cap);
static unsigned long ext4_alloc_inode(ext4_fs_t *fs, unsigned long parent_ino,
				      int is_dir);
static int ext4_create_inode(ext4_fs_t *fs, unsigned long ino, unsigned mode,
			     unsigned uid, unsigned gid);
static void ext4_free_inode(ext4_fs_t *fs, unsigned long ino, int is_dir);
static void ext4_abandon_new_inode(ext4_fs_t *fs, unsigned long ino,
				   int is_dir);
static void ext4_init_owner(unsigned puid, unsigned pgid, unsigned pmode,
			    int is_dir, unsigned *uid, unsigned *gid,
			    unsigned *mode);
static int ext4_dir_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long child_ino,
			unsigned ftype);
static void ext4_free_blocks_from(ext4_fs_t *fs, unsigned long ino,
				  ext4_inode *in, unsigned long from);
static void ext4_flush_meta(ext4_fs_t *fs);

static int ext4_open_impl(ext4_fs_t *fs, const char *path, int flags, unsigned cmode,
			  vfs_file_t **out)
{
	if (!out || !path || !path[0] || !fs)
		return ST_INVALID;

	unsigned long ino = 0;
	int rr = ext4_resolve(fs, path, &ino);
	/* O_CREAT|O_EXCL fails on a name that exists -- a final symlink
	 * counts as one, wherever it leads.  Decided here, under the I/O lock
	 * that the create below holds too (ext4_open_locked), so two such
	 * opens of one name cannot both succeed. */
	if ((flags & O_CREAT) && (flags & O_EXCL)) {
		unsigned long lino = 0;

		if (rr == ST_OK ||
		    (rr == ST_NOT_FOUND &&
		     ext4_resolve_ex(fs, fs->st->cwd_ino, path, 0, &lino, 0) ==
			     ST_OK))
			return ST_EXISTS;
	}
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
		if (ext4_resolve_parent(fs, path, &parent, nm, sizeof(nm)) !=
		    ST_OK)
			return ST_NOT_FOUND;
		unsigned nl = 0;
		while (nm[nl])
			nl++;
		/* Owner/group of the new file per the reference (the creating process). */
		unsigned puid = 0, pgid = 0, pmode = 0;
		ext4_inode pin0;
		if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
			puid = ext4_inode_uid(&pin0);
			pgid = ext4_inode_gid(&pin0);
			pmode = pin0.i_mode;
		}
		/* The mode the caller asked for, not a fixed 0644.  The
		 * set-id bits are deliberately NOT taken from an O_CREAT
		 * open: the new file's group comes from the parent directory
		 * (set-gid directory inheritance), so honouring S_ISGID here
		 * would let a caller create a file set-gid to a group it does
		 * not belong to.  A program that genuinely wants a set-id bit
		 * uses chmod(), which checks that. */
		unsigned nuid = puid, ngid = pgid,
			 nmode = S_IFREG | (cmode & 0777);
		ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);
		unsigned long newino = ext4_alloc_inode(fs, parent, 0);
		if (newino == 0)
			return ST_NOMEM;
		if (ext4_create_inode(fs, newino, nmode, nuid, ngid) != ST_OK) {
			ext4_abandon_new_inode(fs, newino, 0);
			return ST_IO;
		}
		if (ext4_dir_add(fs, parent, nm, nl, newino,
				 EXT4_FT_REG_FILE) != ST_OK) {
			ext4_abandon_new_inode(fs, newino, 0);
			return ST_IO;
		}
		ext4_inode_cache_flush(fs);
		ino = newino;
	}

	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;

	unsigned mode = in.i_mode;

	/* O_TRUNC on an existing regular file: discard its data. */
	if ((flags & O_TRUNC) && (mode & S_IFMT) == S_IFREG &&
	    ext4_inode_size(&in) > 0 && !ext4_is_ro(fs)) {
		/* Cache first, blocks second -- same ordering rule as
		 * ext4_truncate_impl(), and for the same reason: a dirty page
		 * left pointing at a freed block is either unstorable for ever
		 * or written into whatever owns that block next. */
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		ext4_free_blocks_from(fs, ino, &in, 0);
		in.i_size_lo = 0;
		in.i_size_high = 0;
		ext4_stamp_now(fs, &in, EXT4_T_MTIME | EXT4_T_CTIME);
		ext4_inode_cache_flush(fs);
		/* Superseded by the truncation -- see ext4_truncate_impl. */
		ext4_di_clear(fs, ino);
		ext4_write_inode_struct(fs, ino, &in);
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		icache_chain_invalidate(&fs->sb, EXT4_BID_ENC(ino, 0));
	}

	/* Flush any deferred GDT/superblock updates from create/truncate
	 * alloc — but only on the exclusive (mutating-open) path; a plain
	 * shared open must not write metadata.  Deferred updates flush on
	 * the next mutator/fsync/commit anyway. */
	if (fs->meta_dirty && ext4_meta_held_excl(fs))
		ext4_flush_meta(fs);

	ext4_file_t *ef = (ext4_file_t *)kalloc(sizeof(ext4_file_t));
	if (!ef)
		return ST_NOMEM;
	mm_memset(ef, 0, sizeof(*ef));
	ef->fs = fs;
	ef->ino = ino;
	ef->mode = mode;
	ef->size = ext4_inode_size(&in);
	ef->pos = (flags & O_APPEND) ? ef->size : 0;
	ef->is_dir = ((mode & S_IFMT) == S_IFDIR);
	ef->vfs.ops = &ext4_vfs_ops;
	ef->vfs.fs_private = ef;

	if (!ef->is_dir && (mode & S_IFMT) == S_IFREG) {
		/* Attach an icache entry keyed by the encoded chain id so the
         * generic chain cache hangs off the same inode. */
		ef->inode = icache_get(&fs->sb, EXT4_BID_ENC(ino, 0), ef->size, mode, 0,
				       0, 0, 0, 0);
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
	st->st_dev = 0;
	st->st_ino = ino;
	st->st_mode = in.i_mode;
	st->st_nlink = in.i_links_count;
	st->st_uid = ext4_inode_uid(&in);
	st->st_gid = ext4_inode_gid(&in);
	st->st_size = ext4_inode_size(&in);
	if (S_ISCHR(in.i_mode) || S_ISBLK(in.i_mode)) {
		/* The device number, in either encoding (ext4_mknod_impl). */
		uint32_t w[2];
		mm_memcpy(w, in.i_block, sizeof(w));
		if (w[0])
			st->st_rdev = kdev_encode((w[0] >> 8) & 0xffu,
						  w[0] & 0xffu);
		else
			st->st_rdev = kdev_encode(
				(w[1] & 0xfff00u) >> 8,
				(w[1] & 0xffu) | ((w[1] >> 12) & 0xfff00u));
	}
	st->st_blksize = fs->block_size;
	st->st_blocks = in.i_blocks_lo; /* in 512-byte units */
	ext4_time_decode(in.i_atime, in.i_atime_extra,
			 ext4_inode_has_extra(fs, &in,
					      __builtin_offsetof(ext4_inode,
								 i_atime_extra)),
			 &st->st_atime, &st->st_atime_nsec);
	ext4_time_decode(in.i_mtime, in.i_mtime_extra,
			 ext4_inode_has_extra(fs, &in,
					      __builtin_offsetof(ext4_inode,
								 i_mtime_extra)),
			 &st->st_mtime, &st->st_mtime_nsec);
	ext4_time_decode(in.i_ctime, in.i_ctime_extra,
			 ext4_inode_has_extra(fs, &in,
					      __builtin_offsetof(ext4_inode,
								 i_ctime_extra)),
			 &st->st_ctime, &st->st_ctime_nsec);
	return ST_OK;
}

static int ext4_stat_vfs_impl(ext4_fs_t *fs, const char *path, struct kstat *st)
{
	if (!st || !path || !fs)
		return ST_INVALID;
	unsigned long ino = 0;
	int rr = ext4_resolve(fs, path, &ino);
	if (rr != ST_OK) /* propagate ST_IO, not ENOENT */
		return rr;
	return ext4_stat_fill(fs, ino, st);
}

/* The read itself, at an offset the CALLER names.
 *
 * Split out from the descriptor's read so that a caller which has no
 * descriptor position -- demand paging is the one that matters -- can read
 * without touching one.  The reference draws the same line: a page fault goes
 * to the page cache by index and never consults the open file's offset, which
 * is a property of the descriptor and nothing to do with the mapping.
 *
 * Doing it the other way round is what this replaces.  Paging used to seek the
 * shared handle to the page's offset, read, and seek back -- and a mapping's
 * handle IS the caller's open file (mmap references it, fork shares it), so
 * any read() or lseek() on that descriptor while a fault was in flight moved
 * the position out from under it.  The fault then filled the page from
 * somewhere else in the file.  For a library's text that is a page of code
 * that is not the code that belongs there, and the process dies branching
 * into it -- arbitrarily later, and nowhere near the read that caused it.
 *
 * `ra' is the readahead state to use and update; a positioned read passes its
 * own so it cannot disturb the descriptor's sequential-access tracking. */
static long ext4_read_at_impl(vfs_file_t *f, void *buf, long bytes,
			      unsigned long pos, pc_readahead_t *ra)
{
	if (!f || !buf)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || ef->is_dir)
		return ef && ef->is_dir ? -EISDIR : ST_INVALID;
	if (bytes < 0)
		return ST_INVALID;
	if (pos >= ef->size)
		return 0;
	if ((unsigned long)bytes > ef->size - pos)
		bytes = (long)(ef->size - pos);

	/* Read-your-writes for the data write-back buffer is handled by the
	 * ext4_read entry point (flushes under the exclusive lock when the
	 * buffer holds this file's blocks) — this impl runs under the inode
	 * lock SHARED and the pagecache's mapping lock only. */

	unsigned long remaining = (unsigned long)bytes;
	unsigned long copied = 0;
	unsigned long chain_id = EXT4_BID_ENC(ef->ino, 0);

	smap_disable();
	while (remaining) {
		unsigned long page_idx = pos / PAGE_SIZE;
		unsigned page_off = pos % PAGE_SIZE;
		unsigned avail = PAGE_SIZE - page_off;
		unsigned chunk =
			(remaining < avail) ? (unsigned)remaining : avail;

		/* Held across the copy: reclaim cannot free it meanwhile. */
		pc_page_t *pg = pagecache_get_held(chain_id, page_idx, ef->size,
						   &ef->fs->sb, chain_id);
		if (!pg || !pg->data) {
			pagecache_put(pg);
			smap_enable();
			return copied ? (long)copied : ST_IO;
		}
		mm_memcpy((uint8_t *)buf + copied, pg->data + page_off, chunk);
		pagecache_put(pg);

		pagecache_readahead(ra, chain_id, page_idx, ef->size,
				    &ef->fs->sb, chain_id);

		pos += chunk;
		copied += chunk;
		remaining -= chunk;
	}
	smap_enable();
	return (long)copied;
}

/* The descriptor's read: the same thing at the descriptor's own position,
 * advancing it and keeping its readahead state. */
static long ext4_read_impl(vfs_file_t *f, void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	pc_readahead_t ra;
	long r;

	if (!ef)
		return ST_INVALID;
	ra.last_page_index = ef->ra_last_page;
	ra.sequential_count = ef->ra_seq_count;
	ra.ra_pages = ef->ra_pages;
	r = ext4_read_at_impl(f, buf, bytes, ef->pos, &ra);
	if (r > 0) {
		ef->pos += (unsigned long)r;
		ef->ra_last_page = ra.last_page_index;
		ef->ra_seq_count = ra.sequential_count;
		ef->ra_pages = ra.ra_pages;
	}
	return r;
}

static long ext4_seek_impl(vfs_file_t *f, long offset, int whence)
{
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef)
		return ST_INVALID;
	long base;
	switch (whence) {
	case 0:
		base = 0;
		break; /* SEEK_SET */
	case 1:
		base = (long)ef->pos;
		break; /* SEEK_CUR */
	case 2:
		base = (long)ef->size;
		break; /* SEEK_END */
	default:
		return ST_INVALID;
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
	case EXT4_FT_REG_FILE:
		return DT_REG;
	case EXT4_FT_DIR:
		return DT_DIR;
	case EXT4_FT_CHRDEV:
		return DT_CHR;
	case EXT4_FT_BLKDEV:
		return DT_BLK;
	case EXT4_FT_FIFO:
		return DT_FIFO;
	case EXT4_FT_SOCK:
		return DT_SOCK;
	case EXT4_FT_SYMLINK:
		return DT_LNK;
	default:
		return DT_UNKNOWN;
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

	ext4_inode din;
	if (!ext4_get_inode_cached(fs, ef->ino, &din))
		return ST_IO;
	unsigned long dsize = ext4_inode_size(&din);
	uint32_t gen = din.i_generation; /* for the leaf csum */

	uint8_t *blk = (uint8_t *)ext4_bget(fs);
	if (!blk)
		return ST_NOMEM;

	unsigned out_off = 0;
	while (ef->dir_pos < dsize) {
		unsigned long b = ef->dir_pos / fs->block_size;
		unsigned in_blk = ef->dir_pos % fs->block_size;
		unsigned long pbn = ext4_block_map(fs, ef->ino, b);
		if (pbn == 0) {
			ef->dir_pos = (b + 1) *
				      fs->block_size; /* skip sparse block */
			continue;
		}
		if (ext4_read_block(fs, pbn, blk) != ST_OK) {
			ef->dir_pos = (b + 1) * fs->block_size;
			continue;
		}
		if (!ext4_mbc_verified(fs, pbn)) {
			if (!ext4_dir_csum_ok(fs, ef->ino, gen, blk)) {
				ext4_bput(fs, blk);
				ext4_fs_error(fs,
					      "directory leaf checksum mismatch",
					      ef->ino);
				return ST_IO;
			}
			ext4_mbc_mark_verified(fs, pbn);
		}
		int block_done = 0;
		while (in_blk + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de =
				(ext4_dir_entry_2 *)(blk + in_blk);
			unsigned rec = de->rec_len;
			if (rec < 8 || in_blk + rec > fs->block_size) {
				WARN_ON_ONCE(1);
				block_done = 1;
				break;
			}
			if (de->inode != 0 && de->name_len != 0) {
				unsigned namelen = de->name_len;
				unsigned reclen = (19 + namelen + 1 + 7) &
						  ~7u; /* align 8 */
				if (out_off + reclen > (unsigned)bytes) {
					/* Output buffer full; resume here next call. */
					ext4_bput(fs, blk);
					return out_off ? (long)out_off :
							 -EINVAL;
				}
				/* SMAP-aware write to the user buffer: without STAC these
                 * supervisor writes to a user page fault on SMAP-capable CPUs
                 * (real hw / VMware), and since the page is present+writable
                 * the fault handler finds nothing to fix and re-faults forever
                 * (silent hang).  Matches fat32_write_dirent64. */
				struct dirent64 *ld =
					(struct dirent64 *)((uint8_t *)
									  buf +
								  out_off);
				smap_disable();
				ld->d_ino = de->inode;
				ld->d_off = (int64_t)(ef->dir_pos + rec);
				ld->d_reclen = (uint16_t)reclen;
				ld->d_type =
					(uint8_t)ext4_ft_to_dt(de->file_type);
				mm_memcpy(ld->d_name, de->name, namelen);
				ld->d_name[namelen] = '\0';
				smap_enable();
				out_off += reclen;
			}
			in_blk += rec;
			ef->dir_pos += rec;
		}
		if (block_done) {
			/* Advance to the next block boundary. */
			ef->dir_pos = (b + 1) * fs->block_size;
		}
	}
	ext4_bput(fs, blk);
	return (long)out_off;
}

/* The last close of an inode unlinked while open (IC_ORPHAN, see
 * ext4_unlink_impl): now nothing can reach it any more, so its blocks and
 * its number go back, exactly the sequence unlink runs for a file nobody
 * had open.  Under the I/O lock like every other metadata change; a
 * read-only mount leaves the orphan for fsck, which is what the reference
 * does too. */
static void ext4_orphan_reap(ext4_fs_t *fs, unsigned long ino)
{
	ext4_inode in;

	if (!fs || ino == 0 || ext4_is_ro(fs))
		return;
	ext4_io_lock(fs);
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) == ST_OK &&
	    in.i_links_count == 0 && in.i_dtime == 0) {
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		ext4_free_blocks_from(fs, ino, &in, 0);
		in.i_dtime = (uint32_t)timer_get_epoch();
		in.i_size_lo = 0;
		in.i_size_high = 0;
		ext4_di_clear(fs, ino);
		ext4_write_inode_struct(fs, ino, &in);
		ext4_free_inode(fs, ino, 0);
		icache_remove(&fs->sb, EXT4_BID_ENC(ino, 0));
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
		ext4_inode_cache_flush(fs);
		ext4_flush_meta(fs);
	}
	ext4_io_unlock(fs);
}

static int ext4_close_impl(vfs_file_t *f)
{
	if (!f)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (ef) {
		ext4_fs_t *fs = ef->fs;
		unsigned long ino = ef->ino;
		int reap = 0;
		if (ef->inode)
			reap = icache_unref_flagged((ic_inode_t *)ef->inode,
						    IC_ORPHAN);
		kfree(ef);
		if (reap)
			ext4_orphan_reap(fs, ino);
	}
	return ST_OK;
}

static int ext4_chdir_impl(ext4_fs_t *fs, const char *path)
{
	if (!path || !fs)
		return ST_INVALID;
	unsigned long ino = 0;
	int rr = ext4_resolve(fs, path, &ino);
	if (rr != ST_OK) /* propagate ST_IO, not ENOENT */
		return rr;
	ext4_inode in;
	if (!ext4_get_inode_cached(fs, ino, &in) ||
	    (in.i_mode & S_IFMT) != S_IFDIR)
		return -ENOTDIR;
	fs->st->cwd_ino = ino;
	return ST_OK;
}

/* ===================================================================
 * Write support — allocation, metadata writeback, file lifecycle.
 *
 * Metadata (inodes, bitmaps, group descs, directory blocks) is written via
 * ext4_write_block (write-through to the metadata cache).  File data bypasses
 * that cache and uses raw sector I/O, with the pagecache invalidated after a
 * write so reads re-fetch fresh data.  All of this runs under ext4_io_lock.
 * Perf note: metadata writeback here is eager (not batched the way fat32
 * defers FAT-sector flushes).
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
		unsigned long chunk = (count > EXT4_MAX_SECTORS_PER_READ) ?
					      EXT4_MAX_SECTORS_PER_READ :
					      count;
		int st = bdev->write((block_device_t *)bdev, lba, chunk,
				     (const uint8_t *)buf + off);
		if (st != ST_OK)
			return st;
		lba += chunk;
		off += chunk * ss;
		count -= chunk;
	}
	return ST_OK;
}

/* Write a metadata block straight to its final location, keeping any cached
 * copy coherent.  Bypasses the journal — used for checkpointing and outside
 * of transactions. */
static int ext4_write_block_direct(ext4_fs_t *fs, unsigned long pbn,
				   const void *buf)
{
	if (pbn == 0)
		return ST_INVALID;
	if (fs->read_only) /* error latch / read-only mount: refuse writes */
		return ST_ROFS;
	unsigned long lba = fs->part_lba_offset + pbn * fs->sectors_per_block;
	int st = ext4_write_sectors(fs->bdev, lba, fs->sectors_per_block, buf);
	if (st != ST_OK)
		return st;
	{
		/* Refresh EVERY cached copy, not just the first match (mirror of
		 * ext4_mbc_drop).  Refreshing only the first left a duplicate
		 * entry stale forever — the delayed-revert corruption (see the
		 * dedupe comment in ext4_mbc_insert).  ext4_mbc_insert now
		 * dedupes, so >1 match means duplicate creation slipped through
		 * some path — surface it. */
		int matches = 0;
		uint64_t flags;
		spin_lock_irqsave(&fs->st->mbc_lock, &flags);
		for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
			if (fs->st->mbc[i].pbn == pbn && fs->st->mbc[i].data) {
				mm_memcpy(fs->st->mbc[i].data, buf, fs->block_size);
				fs->st->mbc[i].verified = 0; /* content changed */
				matches++;
			}
		spin_unlock_irqrestore(&fs->st->mbc_lock, flags);
		WARN_ON_ONCE(
			matches >
			1); /* duplicate mbc entries for one pbn: insert dedup bypassed */
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
	if (fs->st->txn.active && fs && fs->j_enabled) {
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

/* ---- Device cache-flush barrier -------------------------------------------
 *
 * The journal's crash-safety is an ordering argument — commit durable before
 * checkpoint, checkpoint durable before s_start=0 — and each "durable" step is
 * one bdev->sync (SCSI SYNCHRONIZE CACHE 10 on USB MSD).
 *
 * Not every device implements that command.  VMware's virtual USB storage
 * refuses it with ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE; QEMU
 * honours it.  A refusal is not automatically unsafe: a device with no volatile
 * write cache has nothing to flush, and its writes are already durable when the
 * command completes, so the ordering above still holds without any flush.  Only
 * a device that caches AND refuses to flush is unsafe, and we cannot tell those
 * apart from here (such devices tend not to implement MODE SENSE either, and
 * probing it wedges VMware's bulk endpoint).
 *
 * So: probe once at mount and simply stop re-issuing a command the device has
 * already refused — the callers all used to discard the result anyway, which
 * cost a failing USB round-trip on every barrier. */

static int ext4_dev_sync(ext4_fs_t *fs, const char *where)
{
	if (!fs || !fs->bdev || !fs->bdev->sync || fs->st->flush_unsupported)
		return ST_OK;
	int rc = fs->bdev->sync((block_device_t *)fs->bdev);
	if (rc != ST_OK)
		WARN_RATELIMIT(1, "ext4: device flush failed (%s, rc=%d)", where,
			       rc);
	return rc;
}

/* Probe cache-flush support once, before any barrier can run, and latch it. */
static void ext4_probe_dev_sync(ext4_fs_t *fs)
{
	fs->st->flush_unsupported = 0;
	if (!fs || !fs->bdev || !fs->bdev->sync) {
		fs->st->flush_unsupported = 1;
		return;
	}
	if (fs->bdev->sync((block_device_t *)fs->bdev) != ST_OK) {
		fs->st->flush_unsupported = 1;
		kprintf("ext4: device implements no cache flush; relying on write completion for ordering\n");
	}
}

/* Record a file system error in the superblock: clear the "clean" bit, set
 * the error bit (so the next e2fsck runs a full check), bump the count, and
 * persist it.  ext4_write_super is intentionally NOT gated by the read-only
 * latch so this can persist the mark. */
static void ext4_record_error(ext4_fs_t *fs)
{
	fs->sb_copy.s_state &= ~(uint16_t)EXT4_VALID_FS;
	fs->sb_copy.s_state |= (uint16_t)EXT4_ERROR_FS;
	fs->sb_copy.s_error_count++;
	ext4_write_super(fs); /* best-effort; ungated by latch */
	ext4_dev_sync(fs, "error-mark");
}

/* Central metadata-corruption handler — the ext4_error() analog.  Records the
 * error in the superblock (clears the "clean" bit, sets the error bit, bumps the
 * count) and persists it, then applies the errors= policy: remount read-only
 * (latch fs->read_only so all further writes are refused), panic, or continue.
 * The specific failed op still returns -EIO to its caller; this governs the
 * filesystem-wide reaction.  The policy applies on EVERY error, also on a disk
 * that was already marked errored when it was mounted (the mark says an
 * earlier mount saw an error, not that this one has reacted); only the
 * "read-only" message is once per mount.  ext4_write_super is intentionally
 * NOT gated by the read-only latch so this can persist the mark. */
static void ext4_fs_error(ext4_fs_t *fs, const char *what, unsigned long ino)
{
	if (!fs)
		return;
	if (ino)
		kprintf("ext4: ERROR: %s (inode %lu)\n", what, ino);
	else
		kprintf("ext4: ERROR: %s\n", what);

	ext4_record_error(fs);

	switch (fs->errors_behavior) {
	case EXT4_ERRORS_PANIC:
		kprintf("ext4: errors=panic -> halting\n");
		for (;;)
			__asm__ volatile("cli; hlt");
	case EXT4_ERRORS_CONTINUE:
		kprintf("ext4: errors=continue -> filesystem left writable\n");
		break;
	case EXT4_ERRORS_RO:
	default:
		if (fs->read_only)
			break; /* already refusing writes */
		fs->read_only = 1; /* latch: refuse further writes  */
		kprintf("ext4: remounting filesystem read-only\n");
		break;
	}
}

static int ext4_write_gd(ext4_fs_t *fs, unsigned int group);

/* Flush deferred group-descriptor + superblock free-count updates.  These are
 * advisory (e2fsck can recompute them), so instead of writing them on every
 * allocation we batch them once per top-level write operation. */
/* Group-descriptor dirty tracking: which groups changed since the last flush.
 * Writing ALL group descriptors on every allocating op is fine for a 1-group
 * (tiny) fs but catastrophic on a large multi-hundred-group fs (it would journal
 * ~one block per group per op).  We instead remember the touched groups (a small
 * per-group flag, bounded by a [lo,hi] span) and write only those.  Serialized by
 * the I/O mutex like the rest of the metadata state. */

/* Persistent, physically-contiguous buffer for coalescing file-data writes into
 * DMA-sized transfers (allocated once at mount, reused under the I/O mutex). */

/* Persistent scratch block for ext4_write_gd (allocated once at mount).  A
 * per-call kalloc here could fail under memory pressure and SILENTLY drop a
 * group-descriptor write — leaving the on-disk GD (free count + bitmap csum)
 * stale relative to the just-written bitmap.  All callers hold the I/O mutex, so
 * one shared buffer is safe. */

/* ---- Write-back batching of file data ------------------------------------
 * File-data writes are accumulated into the large physically-contiguous buffer
 * st->wbounce and flushed to disk as ONE big device transfer (ext4_wb_flush),
 * coalescing many small write()s into a few large USB commands — the USB
 * per-command latency, not bandwidth, is the throughput limiter, so fewer/larger
 * commands is the whole win.  The buffer holds a single contiguous physical run
 * [st->wb_pbn, st->wb_pbn+st->wb_len) of inode st->wb_ino.
 *
 * data=ordered is preserved: the buffer is flushed (and made durable) BEFORE the
 * journal commits the metadata that references those blocks (ext4_journal_flush),
 * and before any read of them (ext4_read_impl) or free of them (free run).  All
 * accesses are under the I/O mutex, so no extra locking is needed.  Used only when
 * journalling is on; with no journal, data is written directly as before. */

/* Flush the write-back buffer to its physical home (no device sync here — the
 * caller adds one where ordering requires it).  Returns ST_OK if nothing pending
 * or the write succeeded. */
static int ext4_wb_flush(ext4_fs_t *fs)
{
	if (!fs || fs->st->wb_len == 0)
		return ST_OK;
	unsigned len = fs->st->wb_len;
	unsigned long pbn = fs->st->wb_pbn;
	fs->st->wb_len = 0; /* reset before I/O (non-reentrant under the lock) */
	int st = ext4_write_sectors(
		fs->bdev, fs->part_lba_offset + pbn * fs->sectors_per_block,
		(unsigned long)len * fs->sectors_per_block, fs->st->wbounce);
	if (st != ST_OK) {
		fs->st->wb_err = st;
		WARN_ON_ONCE(1);
	}
	return st;
}

static void ext4_gd_dirty(ext4_fs_t *fs, unsigned g)
{
	fs->meta_dirty = 1;
	if ((!fs->st->gd_dirty || fs->st->gd_dirty_cap < fs->groups_count) &&
	    fs->groups_count) {
		uint8_t *nd = (uint8_t *)kalloc(fs->groups_count);
		if (nd) { /* one-time per mount (groups_count stable) */
			mm_memset(nd, 0, fs->groups_count);
			if (fs->st->gd_dirty)
				kfree(fs->st->gd_dirty);
			fs->st->gd_dirty = nd;
			fs->st->gd_dirty_cap = fs->groups_count;
		}
	}
	if (fs->st->gd_dirty && g < fs->st->gd_dirty_cap)
		fs->st->gd_dirty[g] = 1;
	if (g < fs->st->gd_lo)
		fs->st->gd_lo = g;
	if (g > fs->st->gd_hi)
		fs->st->gd_hi = g;
	if (g < fs->st->gd_seen_lo)
		fs->st->gd_seen_lo = g; /* ever-touched span (for the sync resync) */
	if (g > fs->st->gd_seen_hi)
		fs->st->gd_seen_hi = g;
}

/* Self-heal at sync (defined after the bitmap helpers below). */
static void ext4_resync_gd_from_bitmaps(ext4_fs_t *fs);

static void ext4_flush_meta(ext4_fs_t *fs)
{
	if (!fs->meta_dirty)
		return;
	if (fs->st->gd_lo <= fs->st->gd_hi) { /* write only the groups that changed */
		unsigned long per_block = fs->block_size / fs->desc_size;
		unsigned long last_blk = (unsigned long)-1;
		for (unsigned g = fs->st->gd_lo; g <= fs->st->gd_hi && g < fs->groups_count;
		     g++) {
			if (fs->st->gd_dirty && !fs->st->gd_dirty[g])
				continue; /* precise: skip clean groups */
			/* ext4_write_gd rewrites the entire descriptor block from fs->gdt[],
             * so one call per distinct block covers every dirty group in it. */
			unsigned long blk = (per_block ? g / per_block : 0);
			if (blk != last_blk) {
				ext4_write_gd(fs, g);
				last_blk = blk;
			}
			if (fs->st->gd_dirty)
				fs->st->gd_dirty[g] = 0;
		}
	}
	fs->st->gd_lo = (unsigned)-1;
	fs->st->gd_hi = 0;
	/* The superblock is no longer written per op — it rides the batch flush
     * (ext4_checkpoint) so it never costs a device write per allocating op. */
	fs->meta_dirty = 0;
}

/* Persist the in-memory group descriptor for `group` back to its GDT block. */
static int ext4_write_gd(ext4_fs_t *fs, unsigned int group)
{
	unsigned long gdt_start = fs->first_data_block + 1;
	unsigned long per_block = fs->block_size / fs->desc_size;
	unsigned long blk = gdt_start + group / per_block;
	uint8_t *buf = fs->st->gd_buf, *buf_owned = 0;
	if (!buf) {
		buf_owned = (uint8_t *)kalloc(fs->block_size);
		buf = buf_owned;
	}
	if (!buf)
		return ST_NOMEM;
	/* Rebuild the WHOLE descriptor block from the authoritative in-memory GDT
     * (fs->gdt[]) instead of read-modify-writing just this one descriptor.
     * Many groups share a descriptor block; an RMW re-reads the OTHER descriptors
     * from the cache/disk, which can be stale right after a checkpoint emptied the
     * batch (ext4_write_block_direct only refreshes already-cached entries), and
     * writing them back would clobber another group's just-persisted free count +
     * bitmap checksum — exactly the group-0/group-1 corruption e2fsck reported.
     * fs->gdt[] is always current, so reconstructing the block is both correct
     * and immune to any cache/disk staleness. */
	mm_memset(buf, 0, fs->block_size);
	unsigned long first =
		(blk - gdt_start) * per_block; /* first group in this block */
	unsigned copy = fs->desc_size < sizeof(ext4_group_desc) ?
				fs->desc_size :
				sizeof(ext4_group_desc);
	for (unsigned long k = 0; k < per_block; k++) {
		unsigned long g = first + k;
		if (g >= fs->groups_count)
			break;
		if (fs->has_metadata_csum)
			fs->gdt[g].bg_checksum =
				ext4_gd_csum(fs, (uint32_t)g, &fs->gdt[g]);
		mm_memcpy(buf + k * fs->desc_size, &fs->gdt[g], copy);
	}
	int st = ext4_write_block(fs, blk, buf);
	if (buf_owned)
		kfree(buf_owned); /* never free the persistent scratch block */
	return st;
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

static unsigned long ext4_gd_block_bitmap(ext4_fs_t *fs, unsigned g)
{
	unsigned long lo = fs->gdt[g].bg_block_bitmap_lo;
	unsigned long hi =
		(fs->desc_size >= 64) ? fs->gdt[g].bg_block_bitmap_hi : 0;
	return lo | (hi << 32);
}
static unsigned long ext4_gd_inode_bitmap(ext4_fs_t *fs, unsigned g)
{
	unsigned long lo = fs->gdt[g].bg_inode_bitmap_lo;
	unsigned long hi =
		(fs->desc_size >= 64) ? fs->gdt[g].bg_inode_bitmap_hi : 0;
	return lo | (hi << 32);
}

/* ---- Bitmaps of groups that were never initialized -----------------------
 *
 * With group descriptor checksums, mke2fs (and resize2fs) leave most groups
 * flagged BLOCK_UNINIT and/or INODE_UNINIT and never write their bitmap
 * blocks: whatever those blocks hold -- zeroes on a fresh image, a previous
 * file system's data on a reused disk -- is NOT the bitmap.  The bitmap is
 * defined by the geometry instead:
 *   INODE_UNINIT  every inode free;
 *   BLOCK_UNINIT  used = the group's superblock / GDT / reserved-GDT copy
 *                 plus those of its own block bitmap, inode bitmap and
 *                 inode table blocks that lie inside the group (with flex_bg
 *                 they usually live in another group);
 * and in both, the bits past the end of the group are set (padding).
 *
 * Loading such a bitmap computes it; the first change made to it clears the
 * flag, fixes the free count to the computed value and writes the bitmap
 * with its checksum, all in the same transaction as the change itself, so
 * the descriptor and the bitmap on disk always describe the same state.
 * Only where ext4_uninit_honoured() says so; elsewhere the loaders read the
 * bitmap block as it is. */

/* Set the bitmap bits [start, end): the padding past a group's last block or
 * inode up to the end of the bitmap block. */
static void ext4_mark_bitmap_end(unsigned start, unsigned end, uint8_t *bm)
{
	unsigned i;
	if (start >= end)
		return;
	for (i = start; i < ((start + 7) & ~7u) && i < end; i++)
		ext4_bm_set(bm, i);
	if (i < end)
		mm_memset(bm + (i >> 3), 0xFF, (end - i) >> 3);
}

/* Is physical block `b` inside group `g`? */
static int ext4_block_in_group(const ext4_fs_t *fs, unsigned long b, unsigned g)
{
	if (b < fs->first_data_block)
		return 0;
	return (b - fs->first_data_block) / fs->blocks_per_group == g;
}

/* Status returned by the bitmap loaders when the group's metadata is
 * corrupt (the error has already been recorded through ext4_fs_error); the
 * allocators stop instead of trying the next group. */
#define EXT4_BM_CORRUPT ST_INVALID

/* Compute the block bitmap of BLOCK_UNINIT group `g` into `bm`. */
static int ext4_init_block_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm)
{
	mm_memset(bm, 0, fs->block_size);
	unsigned base = ext4_num_base_meta_blocks(fs, g);
	if ((base >> 3) >= fs->block_size) {
		ext4_fs_error(fs, "group metadata larger than its bitmap", 0);
		return EXT4_BM_CORRUPT;
	}
	for (unsigned b = 0; b < base; b++)
		ext4_bm_set(bm, b);
	unsigned long start = ext4_group_first_block(fs, g);
	unsigned long t = ext4_gd_block_bitmap(fs, g);
	if (ext4_block_in_group(fs, t, g))
		ext4_bm_set(bm, t - start);
	t = ext4_gd_inode_bitmap(fs, g);
	if (ext4_block_in_group(fs, t, g))
		ext4_bm_set(bm, t - start);
	unsigned long itbl = ext4_inode_table_block(fs, &fs->gdt[g]);
	unsigned long itb = ext4_itable_blocks(fs);
	for (t = itbl; t < itbl + itb; t++)
		if (ext4_block_in_group(fs, t, g))
			ext4_bm_set(bm, t - start);
	ext4_mark_bitmap_end(ext4_blocks_in_group(fs, g), fs->block_size * 8,
			     bm);
	return ST_OK;
}

/* Free blocks a bitmap shows within the group's real extent. */
static unsigned long ext4_bitmap_free_blocks(const ext4_fs_t *fs, unsigned g,
					     const uint8_t *bm)
{
	unsigned n = ext4_blocks_in_group(fs, g);
	unsigned long free_cnt = 0;
	for (unsigned b = 0; b < n; b++)
		if (!ext4_bm_test(bm, b))
			free_cnt++;
	return free_cnt;
}

/* Load group `g`'s block bitmap into `bm`: computed for a BLOCK_UNINIT group
 * (*uninit = 1), else read and checksum-verified.  ST_OK, an I/O status, or
 * EXT4_BM_CORRUPT.  Caller holds the group lock. */
static int ext4_load_block_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm,
				  int *uninit)
{
	*uninit = 0;
	if (ext4_uninit_honoured(fs) &&
	    (fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT)) {
		if (g == 0) {
			ext4_fs_error(fs,
				      "block bitmap of group 0 marked uninitialized",
				      0);
			return EXT4_BM_CORRUPT;
		}
		int st = ext4_init_block_bitmap(fs, g, bm);
		if (st == ST_OK)
			*uninit = 1;
		return st;
	}
	int st = ext4_read_block(fs, ext4_gd_block_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st == EXT4_BM_CORRUPT ? ST_IO : st;
	if (!ext4_block_bitmap_csum_ok(fs, g, bm)) {
		ext4_fs_error(fs, "block bitmap checksum mismatch", 0);
		return EXT4_BM_CORRUPT;
	}
	return ST_OK;
}

/* Load group `g`'s inode bitmap into `bm`: all-free (plus padding) for an
 * INODE_UNINIT group (*uninit = 1), else read and checksum-verified. */
static int ext4_load_inode_bitmap(ext4_fs_t *fs, unsigned g, uint8_t *bm,
				  int *uninit)
{
	*uninit = 0;
	if (ext4_uninit_honoured(fs) &&
	    (fs->gdt[g].bg_flags & EXT4_BG_INODE_UNINIT)) {
		if (g == 0) {
			ext4_fs_error(fs,
				      "inode bitmap of group 0 marked uninitialized",
				      0);
			return EXT4_BM_CORRUPT;
		}
		mm_memset(bm, 0, fs->block_size);
		ext4_mark_bitmap_end(fs->inodes_per_group, fs->block_size * 8,
				     bm);
		*uninit = 1;
		return ST_OK;
	}
	int st = ext4_read_block(fs, ext4_gd_inode_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st == EXT4_BM_CORRUPT ? ST_IO : st;
	if (!ext4_inode_bitmap_csum_ok(fs, g, bm)) {
		ext4_fs_error(fs, "inode bitmap checksum mismatch", 0);
		return EXT4_BM_CORRUPT;
	}
	return ST_OK;
}

/* The block bitmap of BLOCK_UNINIT group `g` is about to change: drop the
 * flag and set the descriptor's free count to what the computed bitmap shows
 * (`init_free`), before the caller applies its own delta.  The superblock
 * total moves by the same correction so it keeps equalling the sum of the
 * descriptors.  Caller holds the group lock and writes the bitmap. */
static void ext4_bg_block_uninit_clear(ext4_fs_t *fs, unsigned g,
				       unsigned long init_free)
{
	unsigned long cur = fs->gdt[g].bg_free_blocks_count_lo;
	/* mkfs/resize2fs store exactly this count; a difference means the
	 * descriptor was wrong (fixed here) or the geometry computed above is. */
	WARN_RATELIMIT(cur != init_free,
		       "ext4: group %u BLOCK_UNINIT free count %lu, computed %lu",
		       g, cur, init_free);
	fs->gdt[g].bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
	fs->gdt[g].bg_free_blocks_count_lo = (uint16_t)init_free;
	fs->sb_copy.s_free_blocks_count_lo += (uint32_t)(init_free - cur);
	ext4_gd_dirty(fs, g);
}

/* Write out the computed block bitmap of BLOCK_UNINIT group `g` and clear the
 * flag, with no blocks changing hands: a group that gets an inode in use gets
 * a real block bitmap too.  Caller holds the group lock. */
static int ext4_bg_block_bitmap_materialize(ext4_fs_t *fs, unsigned g)
{
	if (!ext4_uninit_honoured(fs) ||
	    !(fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT))
		return ST_OK;
	uint8_t *bbm = (uint8_t *)ext4_bget(fs);
	if (!bbm)
		return ST_NOMEM;
	int uninit = 0;
	int st = ext4_load_block_bitmap(fs, g, bbm, &uninit);
	if (st == ST_OK && uninit) {
		unsigned long init_free = ext4_bitmap_free_blocks(fs, g, bbm);
		ext4_block_bitmap_csum_set(fs, g, bbm);
		st = ext4_write_block(fs, ext4_gd_block_bitmap(fs, g), bbm);
		if (st == ST_OK)
			ext4_bg_block_uninit_clear(fs, g, init_free);
	}
	ext4_bput(fs, bbm);
	return st;
}

/* Self-heal: recompute every touched group's free-block count + block-bitmap
 * checksum directly from its (authoritative) bitmap, so the on-disk descriptors
 * can never drift from the bitmaps at a sync point — the same principle the mount
 * path applies to the superblock.  Called on sync/fsync/unmount, not per op.
 * Cheap: touched-group bitmaps are hot in the caches, so no extra disk reads. */
static void ext4_resync_gd_from_bitmaps(ext4_fs_t *fs)
{
	if (fs->st->gd_seen_lo > fs->st->gd_seen_hi)
		return;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return;
	int is64 = (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;
	for (unsigned g = fs->st->gd_seen_lo;
	     g <= fs->st->gd_seen_hi && g < fs->groups_count; g++) {
		/* A BLOCK_UNINIT group has no bitmap on disk to resync from:
		 * its count and its computed bitmap agree by definition. */
		if (ext4_uninit_honoured(fs) &&
		    (fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT))
			continue;
		unsigned long bblk = ext4_gd_block_bitmap(fs, g);
		if (ext4_read_block(fs, bblk, bm) != ST_OK)
			continue;
		unsigned long free_cnt = 0;
		for (unsigned long b = 0; b < fs->blocks_per_group; b++)
			if (!ext4_bm_test(bm, b))
				free_cnt++;
		uint32_t cur = fs->gdt[g].bg_free_blocks_count_lo;
		if (is64)
			cur |= (uint32_t)fs->gdt[g].bg_free_blocks_count_hi
			       << 16;
		if (cur !=
		    (uint32_t)
			    free_cnt) { /* drifted: correct it from the bitmap */
			fs->gdt[g].bg_free_blocks_count_lo = (uint16_t)free_cnt;
			if (is64)
				fs->gdt[g].bg_free_blocks_count_hi =
					(uint16_t)(free_cnt >> 16);
		}
		ext4_block_bitmap_csum_set(
			fs, g,
			bm); /* keep the descriptor csum matching the bitmap */
		ext4_gd_dirty(fs, g);
	}
	kfree(bm);
	fs->st->gd_seen_lo = (unsigned)-1;
	fs->st->gd_seen_hi = 0; /* reset: only re-scan groups touched since now */
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
	unsigned long blk = itbl + byte / fs->block_size;
	unsigned off = byte % fs->block_size;
	/* From the block pool, not kalloc: at a 4KB block size kalloc takes the
	 * allocator's large path -- a mapping set up and torn down, the
	 * teardown shooting down the other processors' TLBs -- and this runs on
	 * every metadata update the driver makes.  The pool hands back a buffer
	 * that already exists, and degrades to kalloc only if all of them are
	 * in use. */
	uint8_t *buf = (uint8_t *)ext4_bget(fs);
	if (!buf)
		return ST_NOMEM;
	int st = ext4_read_block(fs, blk, buf);
	if (st != ST_OK) {
		ext4_bput(fs, buf);
		return st;
	}
	unsigned copy = fs->inode_size < sizeof(ext4_inode) ?
				fs->inode_size :
				sizeof(ext4_inode);
	/* INVARIANT: a live directory's i_size only grows (dir blocks are
	 * appended, never truncated; the only legitimate shrink writes are
	 * deletions, which carry i_links_count==0 — rmdir/unlink/rename all
	 * zero size and links in the same write).  Writing a SMALLER size for
	 * a still-linked directory over a larger on-disk one means this RMW is
	 * about to persist a stale inode image — the delayed-revert corruption
	 * (lost htree leaves + lost links_count, e2fsck "bad block number" /
	 * unconnected inodes).  Caught here because this is the generic path
	 * every mkdir/link-count/extent update flows through. */
	{
		const ext4_inode *old = (const ext4_inode *)(buf + off);
		WARN_RATELIMIT((old->i_mode & S_IFMT) == S_IFDIR &&
				       (in->i_mode & S_IFMT) == S_IFDIR &&
				       old->i_links_count != 0 &&
				       in->i_links_count != 0 &&
				       in->i_size_lo < old->i_size_lo,
			       "ext4: live dir ino %lu i_size reverting %u -> %u (links %u -> %u): stale inode RMW",
			       ino, old->i_size_lo, in->i_size_lo,
			       old->i_links_count, in->i_links_count);
	}
	mm_memcpy(buf + off, in, copy);
	ext4_inode_csum_set(fs, ino,
			    buf + off); /* over the full on-disk inode */
	st = ext4_write_block(fs, blk, buf);
	ext4_bput(fs, buf);
	ext4_inode_cache_drop(fs, ino); /* parsed-inode cache now stale */
	/* Whatever was pending for this inode has just been stored -- `in` came
	 * from a read, and every read has the pending change applied to it, so
	 * the bytes that went to the device include it.  Only on success: a
	 * failed write leaves the change still owed, and forgetting it here
	 * would lose it silently. */
	if (st == ST_OK)
		ext4_di_clear(fs, ino);
	return st;
}

/*
 * Store every inode with a pending size/timestamp update.
 *
 * The data comes first, and not as a courtesy: a size that reaches the device
 * before the blocks it covers describes, after a crash, blocks that were never
 * written -- which is to say whatever their previous owner left in them.  So
 * the pages of exactly these files are stored first, in one pass, and only
 * then the inodes.
 *
 * Each store clears its own record (see ext4_write_inode_struct), so the list
 * empties itself and an inode whose write failed stays on it.
 *
 * The caller must hold the metadata lock exclusively: this writes metadata.
 */
static void ext4_di_flush(ext4_fs_t *fs)
{
	unsigned long inos[EXT4_DI_MAX];
	uint64_t sizes[EXT4_DI_MAX];
	unsigned long ids[EXT4_DI_MAX];
	unsigned n = 0;
	uint64_t flags;

	might_sleep();
	if (!fs)
		return;

	spin_lock_irqsave(&fs->st->di_lock, &flags);
	for (unsigned i = 0; i < fs->st->di_n; i++) {
		inos[n] = fs->st->di[i].ino;
		sizes[n] = fs->st->di[i].size;
		n++;
	}
	spin_unlock_irqrestore(&fs->st->di_lock, flags);
	if (n == 0)
		return;

	for (unsigned i = 0; i < n; i++)
		ids[i] = EXT4_BID_ENC(inos[i], 0);
	pagecache_flush_fileset(&fs->sb, ids, n);

	for (unsigned i = 0; i < n; i++) {
		ext4_inode in;

		/* Read brings the pending change back with it, so what is
		 * written is the current state of the file. */
		if (ext4_read_inode_loc(fs, inos[i], &in, 0, 0) != ST_OK)
			continue;
		/* The read must have come back carrying the pending size.
		 *
		 * If it did not, ext4_di_apply is not reaching this path, and
		 * the write below is about to store the size the device
		 * already had and then drop the record -- which is the one way
		 * this design loses a file's length outright.  Nothing else
		 * would report it: the write succeeds, the file simply ends up
		 * shorter than it was.  Whoever holds the lock cannot have
		 * changed it underneath us, so a mismatch is a broken overlay
		 * and not a race. */
		WARN_RATELIMIT(ext4_inode_size(&in) != sizes[i],
			       "ext4: inode %lu pending size %lu not applied on read (got %lu) - deferred size would be lost",
			       inos[i], (unsigned long)sizes[i],
			       ext4_inode_size(&in));
		ext4_write_inode_struct(fs, inos[i], &in);
	}
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
static unsigned long ext4_alloc_inode(ext4_fs_t *fs, unsigned long parent_ino,
				      int is_dir)
{
	unsigned pgroup =
		parent_ino ?
			(unsigned)((parent_ino - 1) / fs->inodes_per_group) :
			0;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return 0;
	for (unsigned gi = 0; gi < fs->groups_count; gi++) {
		unsigned g = (pgroup + gi) % fs->groups_count;
		if (fs->gdt[g].bg_free_inodes_count_lo == 0)
			continue;
		/* Group lock: this group's inode-bitmap read-modify-write. */
		ext4_bg_lock(fs, g);
		unsigned long bblk = ext4_gd_inode_bitmap(fs, g);
		int uninit = 0;
		int lst = ext4_load_inode_bitmap(fs, g, bm, &uninit);
		if (lst == EXT4_BM_CORRUPT) {
			ext4_bg_unlock(fs, g);
			kfree(bm);
			return 0;
		}
		if (lst != ST_OK) {
			ext4_bg_unlock(fs, g);
			continue;
		}
		for (unsigned i = 0; i < fs->inodes_per_group; i++) {
			if (ext4_bm_test(bm, i))
				continue;
			unsigned long ino =
				(unsigned long)g * fs->inodes_per_group + i + 1;
			if (ino < fs->first_ino && ino != EXT4_ROOT_INO)
				continue;
			/* A group that gets an inode in use gets a real
			 * block bitmap as well. */
			if (ext4_bg_block_bitmap_materialize(fs, g) != ST_OK) {
				ext4_bg_unlock(fs, g);
				kfree(bm);
				return 0;
			}
			ext4_bm_set(bm, i);
			ext4_inode_bitmap_csum_set(fs, g, bm);
			/* shrink bg_itable_unused if we used an inode past the tracked front
             * region (mirrors the reference's ext4_new_inode). */
			if (fs->has_metadata_csum) {
				unsigned off1 =
					i +
					1; /* 1-based offset within the group */
				unsigned used_front = fs->inodes_per_group -
						      ext4_itable_unused(fs, g);
				/* none at all in an INODE_UNINIT group, which
				 * is initialized by this allocation */
				if (uninit)
					used_front = 0;
				if (off1 > used_front)
					ext4_itable_unused_set(
						fs, g,
						fs->inodes_per_group - off1);
			}
			if (ext4_write_block(fs, bblk, bm) != ST_OK) {
				ext4_bg_unlock(fs, g);
				kfree(bm);
				return 0;
			}
			if (uninit)
				fs->gdt[g].bg_flags &=
					(uint16_t)~EXT4_BG_INODE_UNINIT;
			fs->gdt[g].bg_free_inodes_count_lo--;
			if (is_dir)
				fs->gdt[g].bg_used_dirs_count_lo++;
			fs->sb_copy.s_free_inodes_count--;
			ext4_gd_dirty(fs, g);
			ext4_bg_unlock(fs, g);
			/* This number almost certainly belonged to a file that
			 * has since been deleted.  Both caches are keyed by
			 * inode number, so whatever is still held under it
			 * describes the DEAD file: the new file would then be
			 * read back as the old one's bytes, at the new file's
			 * own (correctly stat'd) length — the right size, the
			 * wrong contents, and no error anywhere.
			 *
			 * Conventional practice attaches the cache to the
			 * in-core inode, which is torn down with the file, so
			 * a recycled number starts empty by construction.
			 * Keyed by number, the equivalent has to be explicit,
			 * and it belongs HERE rather than only at the points
			 * where an inode is freed: a file unlinked while still
			 * open has its pages faulted straight back in by its
			 * last reader, after the unlink invalidated them.
			 * Allocation is the one point every new file passes
			 * through, whatever became of the previous owner. */
			icache_remove(&fs->sb, EXT4_BID_ENC(ino, 0));
			pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(ino, 0));
			/* Same argument for the names: if this number last
			 * belonged to a directory, whatever is still cached
			 * under it describes that directory's contents, and
			 * the new one would answer lookups with the dead
			 * one's children. */
			ext4_dc_drop_dir(fs, ino);
			/* And for a pending size: it describes the file that
			 * used to have this number.  Left in place it would be
			 * laid over the new file's inode on every read, giving
			 * it the dead one's length. */
			ext4_di_clear(fs, ino);
			kfree(bm);
			return ino;
		}
		ext4_bg_unlock(fs, g);
	}
	kfree(bm);
	return 0;
}

static void ext4_free_block(ext4_fs_t *fs,
			    unsigned long pbn); /* defined below */

static void ext4_free_inode(ext4_fs_t *fs, unsigned long ino, int is_dir)
{
	if (ino == 0 || ino > fs->inodes_count)
		return;
	/* Release the external xattr block (i_file_acl), if any, so deleting a file
     * that carried a large/ACL attribute doesn't leak the block. */
	{
		ext4_inode in;
		if (ext4_read_inode_loc(fs, ino, &in, 0, 0) == ST_OK &&
		    in.i_file_acl_lo)
			ext4_free_block(fs, in.i_file_acl_lo);
	}
	unsigned g = (ino - 1) / fs->inodes_per_group;
	unsigned i = (ino - 1) % fs->inodes_per_group;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return;
	/* Group lock: serialises this group's bitmap read-modify-write. */
	ext4_bg_lock(fs, g);
	unsigned long bblk = ext4_gd_inode_bitmap(fs, g);
	int uninit = 0;
	/* An INODE_UNINIT group loads as all-free: nothing in it to release. */
	if (ext4_load_inode_bitmap(fs, g, bm, &uninit) == ST_OK && !uninit) {
		if (ext4_bm_test(bm, i)) {
			ext4_bm_clear(bm, i);
			ext4_inode_bitmap_csum_set(fs, g, bm);
			ext4_write_block(fs, bblk, bm);
			fs->gdt[g].bg_free_inodes_count_lo++;
			if (is_dir && fs->gdt[g].bg_used_dirs_count_lo)
				fs->gdt[g].bg_used_dirs_count_lo--;
			fs->sb_copy.s_free_inodes_count++;
			ext4_gd_dirty(fs, g);
		}
	}
	ext4_bg_unlock(fs, g);
	kfree(bm);
}

/* Free a contiguous run of data blocks [start, start+len).  For each affected
 * group the bitmap is read, ALL the run's bits cleared, and the GD/super free
 * counts bumped — ONCE per group, not once per block.  Per-block freeing would
 * re-read + re-write the same group bitmap for every block: unlinking a 100 MB
 * file is 25600 single-bit RMWs, a multi-second stall that looks like a hang. */
static void ext4_free_blocks_run(ext4_fs_t *fs, unsigned long start,
				 unsigned long len)
{
	/* If the run being freed overlaps the write-back buffer, flush it first so the
     * buffer never holds data for now-free (and possibly soon-reallocated) blocks. */
	if (fs->st->wb_len > 0 && start < fs->st->wb_pbn + fs->st->wb_len &&
	    fs->st->wb_pbn < start + len)
		ext4_wb_flush(fs);
	uint8_t *bm = 0;
	while (len > 0) {
		if (start < fs->first_data_block) {
			start++;
			len--;
			continue;
		}
		unsigned g = (unsigned)((start - fs->first_data_block) /
					fs->blocks_per_group);
		if (g >= fs->groups_count)
			break;
		unsigned long gbase = fs->first_data_block +
				      (unsigned long)g * fs->blocks_per_group;
		unsigned long i =
			start - gbase; /* first index within group   */
		unsigned long n = fs->blocks_per_group -
				  i; /* blocks of the run in group */
		if (n > len)
			n = len;
		/* A freed block's journal copy must never be replayed over its
		 * next use.  Both copies are in memory only, so forget them --
		 * see ext4_drop_journal_copy(fs).  This used to force a full
		 * durable checkpoint instead, which made every unlink of a
		 * recently-written file wait on the device. */
		for (unsigned long k = 0; k < n; k++)
			ext4_drop_journal_copy(fs, start + k);
		if (!bm) {
			bm = (uint8_t *)kalloc(fs->block_size);
			if (!bm)
				return;
		}
		/* Group lock: this group's block-bitmap read-modify-write. */
		ext4_bg_lock(fs, g);
		unsigned long bblk = ext4_gd_block_bitmap(fs, g);
		int uninit = 0;
		int lst = ext4_load_block_bitmap(fs, g, bm, &uninit);
		if (lst == EXT4_BM_CORRUPT) {
			ext4_bg_unlock(fs, g);
			kfree(bm);
			return;
		}
		if (lst == ST_OK) {
			unsigned long init_free =
				uninit ? ext4_bitmap_free_blocks(fs, g, bm) : 0;
			unsigned long cleared = 0;
			for (unsigned long k = 0; k < n; k++)
				if (ext4_bm_test(bm, i + k)) {
					ext4_bm_clear(bm, i + k);
					cleared++;
				}
			if (cleared) {
				if (uninit)
					ext4_bg_block_uninit_clear(fs, g,
								   init_free);
				ext4_block_bitmap_csum_set(fs, g, bm);
				ext4_write_block(fs, bblk, bm);
				fs->gdt[g].bg_free_blocks_count_lo =
					(uint16_t)(fs->gdt[g]
							   .bg_free_blocks_count_lo +
						   cleared);
				fs->sb_copy.s_free_blocks_count_lo +=
					(uint32_t)cleared;
				ext4_gd_dirty(fs, g);
			}
		}
		ext4_bg_unlock(fs, g);
		for (unsigned long k = 0; k < n; k++)
			ext4_mbc_drop(fs, start +
				      k); /* avoid stale cache if reallocated */
		start += n;
		len -= n;
	}
	if (bm)
		kfree(bm);
}

static void ext4_free_block(ext4_fs_t *fs, unsigned long pbn)
{
	ext4_free_blocks_run(fs, pbn, 1);
}

/* ===================================================================
 * Extent-tree WRITE support — growth at the end, to arbitrary depth.
 *
 * Blocks added past the last extent (a file or directory growing at its
 * end, the common case) go in at the RIGHTMOST path of the tree; anything
 * else -- a hole being filled -- is handed to the general sorted insertion
 * further down (ext4_ext_insert_run).
 * That makes growth a clean "add to the rightmost leaf; on full, build a new
 * rightmost branch under the lowest ancestor with room; if none, grow a new
 * level on top".  External index/leaf blocks carry the et_checksum tail
 * (ext4_ext_block_csum_set); the inline root is covered by the inode csum.
 * Metadata blocks are allocated via ext4_alloc_one_block, so the caller must
 * have already persisted the data-block bitmaps (see the two-pass
 * reserve-then-map allocator) to avoid double-allocation.
 * =================================================================== */
static unsigned long ext4_alloc_one_block(ext4_fs_t *fs); /* defined below */

/* Entries an EXTERNAL extent block holds (reserving the 4-byte csum tail). */
static unsigned ext4_ext_extern_max(const ext4_fs_t *fs)
{
	return (fs->block_size - sizeof(ext4_extent_header) - 4) /
	       sizeof(ext4_extent);
}

/* Stamp the et_checksum tail of an external extent block (no-op w/o csum). */
static void ext4_ext_block_csum_set(const ext4_fs_t *fs, unsigned long ino,
				    uint32_t gen, uint8_t *blk)
{
	if (!fs->has_metadata_csum)
		return;
	const ext4_extent_header *eh = (const ext4_extent_header *)blk;
	unsigned long off = sizeof(ext4_extent_header) +
			    (unsigned long)eh->eh_max * sizeof(ext4_extent);
	if (off + 4 > fs->block_size)
		return;
	uint32_t seed = ext4_inode_csum_seed(fs, ino, gen);
	uint32_t c = ext4_crc32c(seed, blk, off);
	mm_memcpy(blk + off, &c, 4);
}

/* Push the inline root down into a new external block, making the root a single
 * index entry pointing to it (depth++).  Works at any depth (the copied block
 * keeps the root's old depth). */
static int ext4_ext_grow_depth(ext4_fs_t *fs, unsigned long ino, ext4_inode *in)
{
	unsigned bs = fs->block_size;
	uint32_t gen = in->i_generation;
	ext4_extent_header *eh = (ext4_extent_header *)in->i_block;
	unsigned long b = ext4_alloc_one_block(fs);
	if (!b)
		return ST_NOSPC;
	uint8_t *buf = (uint8_t *)kalloc(bs);
	if (!buf) {
		ext4_free_block(fs, b);
		return ST_NOMEM;
	}
	mm_memset(buf, 0, bs);
	unsigned cnt = eh->eh_entries;
	/* The new root's lone index entry covers from the subtree's first logical
     * block (0 for a normal file, the first extent's block for a sparse one).
     * Read it from the still-intact inline root before we overwrite it below. */
	uint32_t first_block;
	mm_memcpy(&first_block, in->i_block + sizeof(ext4_extent_header), 4);
	mm_memcpy(buf, in->i_block,
		  sizeof(ext4_extent_header) +
			  (unsigned long)cnt * sizeof(ext4_extent));
	((ext4_extent_header *)buf)->eh_max = (uint16_t)ext4_ext_extern_max(fs);
	ext4_ext_block_csum_set(fs, ino, gen, buf);
	int st = ext4_write_block(fs, b, buf);
	kfree(buf);
	if (st != ST_OK) {
		ext4_free_block(fs, b);
		return st;
	}
	eh->eh_depth = (uint16_t)(eh->eh_depth + 1);
	eh->eh_entries = 1;
	eh->eh_max = 4;
	ext4_extent_idx *ix = (ext4_extent_idx *)(eh + 1);
	ix[0].ei_block = first_block;
	ix[0].ei_leaf_lo = (uint32_t)b;
	ix[0].ei_leaf_hi = (uint16_t)(b >> 32);
	ix[0].ei_unused = 0;
	in->i_blocks_lo += bs / 512; /* B is a metadata block */
	return ST_OK;
}

/* The rightmost leaf is full: build a fresh rightmost branch holding the new
 * extent (lidx,pbn,chunk).  pbuf[1..depth]/pblk[1..depth] are the descended
 * rightmost external nodes; level 0 is the inline root. */
static int ext4_ext_grow_branch(ext4_fs_t *fs, unsigned long ino,
				ext4_inode *in, uint8_t *pbuf[],
				unsigned long pblk[], unsigned depth,
				unsigned long lidx, unsigned long pbn,
				unsigned chunk)
{
	unsigned bs = fs->block_size;
	uint32_t gen = in->i_generation;
	unsigned emax = ext4_ext_extern_max(fs);

	/* Lowest ancestor (level) with room for one more child pointer. */
	int A = (int)depth - 1;
	while (A >= 0) {
		ext4_extent_header *h =
			(A == 0) ? (ext4_extent_header *)in->i_block :
				   (ext4_extent_header *)pbuf[A];
		if (h->eh_entries < h->eh_max)
			break;
		A--;
	}
	if (A < 0) { /* whole path full: add a new level */
		int rc = ext4_ext_grow_depth(fs, ino, in);
		if (rc != ST_OK)
			return rc;
		A = 0;
		depth = ((ext4_extent_header *)in->i_block)
				->eh_depth; /* grew by 1 */
	}

	/* Allocate the new branch blocks: levels A+1 .. depth (leaf at depth). */
	unsigned nlev = depth - (unsigned)A;
	if (nlev > 8)
		return ST_NOSPC;
	unsigned long nb[8];
	unsigned got = 0;
	for (unsigned i = 0; i < nlev; i++) {
		unsigned long b = ext4_alloc_one_block(fs);
		if (!b) {
			for (unsigned j = 0; j < got; j++)
				ext4_free_block(fs, nb[j]);
			return ST_NOSPC;
		}
		nb[got++] = b;
	}
	uint8_t *buf = (uint8_t *)kalloc(bs);
	if (!buf) {
		for (unsigned j = 0; j < nlev; j++)
			ext4_free_block(fs, nb[j]);
		return ST_NOMEM;
	}

	/* Leaf (deepest new block = nb[nlev-1]). */
	mm_memset(buf, 0, bs);
	{
		ext4_extent_header *h = (ext4_extent_header *)buf;
		h->eh_magic = EXT4_EXT_MAGIC;
		h->eh_entries = 1;
		h->eh_max = (uint16_t)emax;
		h->eh_depth = 0;
		ext4_extent *e = (ext4_extent *)(h + 1);
		e->ee_block = (uint32_t)lidx;
		e->ee_len = (uint16_t)chunk;
		e->ee_start_hi = (uint16_t)(pbn >> 32);
		e->ee_start_lo = (uint32_t)pbn;
		ext4_ext_block_csum_set(fs, ino, gen, buf);
		if (ext4_write_block(fs, nb[nlev - 1], buf) != ST_OK)
			goto io_fail;
	}

	/* Interior new index nodes, level depth-1 .. A+1. */
	for (int lvl = (int)depth - 1; lvl >= A + 1; lvl--) {
		unsigned bi = (unsigned)(lvl - (A + 1));
		unsigned long childblk = nb[bi + 1];
		mm_memset(buf, 0, bs);
		ext4_extent_header *h = (ext4_extent_header *)buf;
		h->eh_magic = EXT4_EXT_MAGIC;
		h->eh_entries = 1;
		h->eh_max = (uint16_t)emax;
		h->eh_depth = (uint16_t)(depth - lvl);
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
		ix->ei_block = (uint32_t)lidx;
		ix->ei_leaf_lo = (uint32_t)childblk;
		ix->ei_leaf_hi = (uint16_t)(childblk >> 32);
		ix->ei_unused = 0;
		ext4_ext_block_csum_set(fs, ino, gen, buf);
		if (ext4_write_block(fs, nb[bi], buf) != ST_OK)
			goto io_fail;
	}
	kfree(buf);

	/* Link the top new node (nb[0]) into the ancestor with room (path[A]). */
	if (A == 0) {
		ext4_extent_header *h = (ext4_extent_header *)in->i_block;
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
		ix[h->eh_entries].ei_block = (uint32_t)lidx;
		ix[h->eh_entries].ei_leaf_lo = (uint32_t)nb[0];
		ix[h->eh_entries].ei_leaf_hi = (uint16_t)(nb[0] >> 32);
		ix[h->eh_entries].ei_unused = 0;
		h->eh_entries++; /* inline root persisted by caller */
	} else {
		ext4_extent_header *h = (ext4_extent_header *)pbuf[A];
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
		ix[h->eh_entries].ei_block = (uint32_t)lidx;
		ix[h->eh_entries].ei_leaf_lo = (uint32_t)nb[0];
		ix[h->eh_entries].ei_leaf_hi = (uint16_t)(nb[0] >> 32);
		ix[h->eh_entries].ei_unused = 0;
		h->eh_entries++;
		ext4_ext_block_csum_set(fs, ino, gen, pbuf[A]);
		if (ext4_write_block(fs, pblk[A], pbuf[A]) != ST_OK)
			return ST_IO;
	}
	in->i_blocks_lo +=
		nlev * (bs / 512); /* all new branch blocks are metadata */
	return ST_OK;
io_fail:
	kfree(buf);
	for (unsigned j = 0; j < nlev; j++)
		ext4_free_block(fs, nb[j]);
	return ST_IO;
}

static unsigned ext4_ext_insert_run(ext4_fs_t *fs, unsigned long ino,
				    ext4_inode *in, unsigned long lblk,
				    unsigned long pblk,
				    unsigned len); /* defined below */

/* Append `addlen` contiguous blocks (lidx,pbn..) to the inode's extent tree,
 * coalescing with the rightmost extent and growing the tree as needed.  Returns
 * the number of blocks actually mapped (== addlen on success, fewer if the tree
 * could not grow).  May allocate metadata blocks (caller flushes data bitmaps). */
static unsigned ext4_ext_add(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			     unsigned long lidx, unsigned long pbn,
			     unsigned addlen)
{
	unsigned bs = fs->block_size;
	uint32_t gen = in->i_generation;
	unsigned done = 0;
	ext4_extent_header *reh = (ext4_extent_header *)in->i_block;
	if (!(in->i_flags & EXT4_INODE_EXTENTS_FL) ||
	    reh->eh_magic != EXT4_EXT_MAGIC) {
		reh->eh_magic = EXT4_EXT_MAGIC;
		reh->eh_entries = 0;
		reh->eh_max = 4;
		reh->eh_depth = 0;
		reh->eh_generation = 0;
		in->i_flags |= EXT4_INODE_EXTENTS_FL;
	}

	while (addlen > 0) {
		unsigned depth = ((ext4_extent_header *)in->i_block)->eh_depth;
		if (depth > 6)
			break; /* read path caps at 6 levels */
		uint8_t *pbuf[8] = { 0 };
		unsigned long pblk[8] = { 0 };
		uint8_t *node = in->i_block;
		int ok = 1;
		for (unsigned d = 0; d < depth; d++) {
			ext4_extent_header *h = (ext4_extent_header *)node;
			unsigned cnt = h->eh_entries;
			if (cnt == 0) {
				ok = 0;
				break;
			}
			ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);
			unsigned long child =
				(unsigned long)ix[cnt - 1].ei_leaf_lo |
				((unsigned long)ix[cnt - 1].ei_leaf_hi << 32);
			uint8_t *cb = (uint8_t *)kalloc(bs);
			if (!cb || ext4_read_block(fs, child, cb) != ST_OK) {
				if (cb)
					kfree(cb);
				ok = 0;
				break;
			}
			pbuf[d + 1] = cb;
			pblk[d + 1] = child;
			node = cb;
		}
		if (!ok) {
			for (unsigned d = 1; d <= depth; d++)
				if (pbuf[d])
					kfree(pbuf[d]);
			break;
		}

		ext4_extent_header *leh = (ext4_extent_header *)node;
		ext4_extent *ex = (ext4_extent *)(leh + 1);
		int is_inline = (depth == 0);
		unsigned chunk = addlen > 32768 ? 32768 : addlen;
		int handled = 0;

		if (leh->eh_entries > 0) {
			ext4_extent *last = &ex[leh->eh_entries - 1];
			unsigned llen = last->ee_len;
			if (llen > 32768)
				llen -= 32768;
			/* Not past the last extent: a hole inside the file
			 * is being filled, and the extent has to go where it
			 * belongs in the order, not at the end. */
			if (lidx < (unsigned long)last->ee_block + llen) {
				for (unsigned d = 1; d <= depth; d++)
					if (pbuf[d])
						kfree(pbuf[d]);
				return done + ext4_ext_insert_run(fs, ino, in,
								  lidx, pbn,
								  addlen);
			}
		}
		if (leh->eh_entries > 0) { /* coalesce with the last extent? */
			ext4_extent *last = &ex[leh->eh_entries - 1];
			unsigned llen = last->ee_len;
			if (llen > 32768)
				llen -= 32768;
			unsigned long lphys =
				(unsigned long)last->ee_start_lo |
				((unsigned long)last->ee_start_hi << 32);
			if (last->ee_block + llen == lidx &&
			    lphys + llen == pbn && llen < 32768) {
				unsigned c = 32768 - llen;
				if (c > addlen)
					c = addlen;
				last->ee_len = (uint16_t)(llen + c);
				chunk = c;
				handled = 1;
			}
		}
		if (!handled &&
		    leh->eh_entries < leh->eh_max) { /* add a new extent */
			ext4_extent *ne = &ex[leh->eh_entries];
			ne->ee_block = (uint32_t)lidx;
			ne->ee_len = (uint16_t)chunk;
			ne->ee_start_hi = (uint16_t)(pbn >> 32);
			ne->ee_start_lo = (uint32_t)pbn;
			leh->eh_entries++;
			handled = 1;
		}

		int rc = ST_OK;
		if (handled) {
			if (!is_inline) {
				ext4_ext_block_csum_set(fs, ino, gen, node);
				rc = ext4_write_block(fs, pblk[depth], node);
			} /* inline root persisted by the caller */
		} else {
			rc = ext4_ext_grow_branch(fs, ino, in, pbuf, pblk,
						  depth, lidx, pbn, chunk);
		}
		for (unsigned d = 1; d <= depth; d++)
			if (pbuf[d])
				kfree(pbuf[d]);
		if (rc != ST_OK)
			break;
		lidx += chunk;
		pbn += chunk;
		addlen -= chunk;
		done += chunk;
	}
	return done;
}

/* Allocate `count` data blocks for inode `in`, appending them as extents
 * starting at logical index `start_lidx`.  Returns the number allocated
 * (may be < count if the group/extent-root fills up). */
/* Allocate up to `count` data blocks for inode `ino`/`in`, appending them as
 * extents starting at logical `start_lidx`.  Returns the number mapped.
 *
 * Two phases so that growing the extent tree (which allocates metadata blocks
 * from the same bitmaps) never races the data-block allocation:
 *   A. grab contiguous runs of free data blocks and WRITE the bitmaps;
 *   B. map the runs into the extent tree (may now safely allocate metadata).
 * Returns < count if free space is too fragmented for one call (the caller
 * re-invokes) or the tree can't grow (ENOSPC). */
#define EXT4_ALLOC_RUNS_MAX 64
static unsigned ext4_alloc_blocks_for_file(ext4_fs_t *fs, unsigned long ino,
					   ext4_inode *in,
					   unsigned long start_lidx,
					   unsigned count)
{
	if (count == 0)
		return 0;
	struct {
		unsigned long pbn;
		unsigned len;
	} runs[EXT4_ALLOC_RUNS_MAX];
	unsigned nruns = 0, phys = 0;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return 0;

	/* First pass: reserve physical data blocks as contiguous runs; persist bitmaps. */
	for (unsigned g = 0; g < fs->groups_count && phys < count &&
			     nruns < EXT4_ALLOC_RUNS_MAX;
	     g++) {
		if (fs->gdt[g].bg_free_blocks_count_lo == 0)
			continue;
		/* Group lock: this group's block-bitmap read-modify-write. */
		ext4_bg_lock(fs, g);
		unsigned long bblk = ext4_gd_block_bitmap(fs, g);
		int uninit = 0;
		int lst = ext4_load_block_bitmap(fs, g, bm, &uninit);
		if (lst == EXT4_BM_CORRUPT) {
			ext4_bg_unlock(fs, g);
			break;
		}
		if (lst != ST_OK) {
			ext4_bg_unlock(fs, g);
			continue;
		}
		unsigned long init_free =
			uninit ? ext4_bitmap_free_blocks(fs, g, bm) : 0;
		unsigned long base = fs->first_data_block +
				     (unsigned long)g * fs->blocks_per_group;
		unsigned group_alloc = 0, i = 0;
		while (i < fs->blocks_per_group && phys < count &&
		       nruns < EXT4_ALLOC_RUNS_MAX) {
			if (ext4_bm_test(bm, i)) {
				i++;
				continue;
			}
			unsigned rstart = i, rlen = 0;
			while (i < fs->blocks_per_group &&
			       !ext4_bm_test(bm, i) && phys < count &&
			       rlen < 32768) {
				ext4_bm_set(bm, i);
				i++;
				rlen++;
				phys++;
				group_alloc++;
			}
			runs[nruns].pbn = base + rstart;
			runs[nruns].len = rlen;
			nruns++;
		}
		if (group_alloc) {
			if (uninit)
				ext4_bg_block_uninit_clear(fs, g, init_free);
			ext4_block_bitmap_csum_set(fs, g, bm);
			ext4_write_block(fs, bblk, bm);
			fs->gdt[g].bg_free_blocks_count_lo -= group_alloc;
			fs->sb_copy.s_free_blocks_count_lo -= group_alloc;
			ext4_gd_dirty(fs, g);
		}
		ext4_bg_unlock(fs, g);
	}
	kfree(bm);

	/* Second pass: map runs into the extent tree (metadata alloc is safe now). */
	unsigned mapped = 0;
	for (unsigned r = 0; r < nruns; r++) {
		unsigned got = ext4_ext_add(fs, ino, in, start_lidx + mapped,
					    runs[r].pbn, runs[r].len);
		mapped += got;
		if (got < runs[r].len) {
			/* Tree couldn't grow: free the unmapped reservations (this run's tail
             * + all later runs) so they aren't leaked. */
			for (unsigned k = got; k < runs[r].len; k++)
				ext4_free_block(fs, runs[r].pbn + k);
			for (unsigned r2 = r + 1; r2 < nruns; r2++)
				for (unsigned k = 0; k < runs[r2].len; k++)
					ext4_free_block(fs, runs[r2].pbn + k);
			break;
		}
	}
	in->i_blocks_lo += mapped * (fs->block_size / 512); /* data blocks */
	return mapped;
}

/* Free every data block of inode `in` from logical index `from` onward, and
 * trim the inline extent tree accordingly.  Used by truncate/unlink. */
/* Free blocks with logical index >= `from` in the subtree whose header is at
 * `node` (an inline root or an external block buffer), compacting the kept
 * entries.  Returns the number of entries kept; accumulates freed blocks (data
 * AND metadata) in *freed so the caller can fix i_blocks. */
static unsigned ext4_ext_free_subtree(ext4_fs_t *fs, unsigned long ino,
				      uint32_t gen, uint8_t *node,
				      unsigned long from, unsigned long *freed)
{
	ext4_extent_header *eh = (ext4_extent_header *)node;
	if (eh->eh_magic != EXT4_EXT_MAGIC)
		return 0;
	unsigned n = eh->eh_entries, keep = 0;

	if (eh->eh_depth == 0) { /* leaf: free data blocks */
		ext4_extent *ex = (ext4_extent *)(eh + 1);
		for (unsigned e = 0; e < n; e++) {
			unsigned long b0 = ex[e].ee_block;
			unsigned len = ex[e].ee_len;
			if (len > 32768)
				len -= 32768;
			unsigned long start =
				(unsigned long)ex[e].ee_start_lo |
				((unsigned long)ex[e].ee_start_hi << 32);
			if (b0 + len <= from) {
				if (keep != e)
					ex[keep] = ex[e];
				keep++;
				continue;
			}
			unsigned long cut = (from > b0) ? (from - b0) : 0;
			if (len > cut) { /* free the run in one pass */
				ext4_free_blocks_run(fs, start + cut,
						     len - cut);
				*freed += (len - cut);
			}
			if (cut > 0) {
				ex[keep] = ex[e];
				ex[keep].ee_len = (uint16_t)cut;
				keep++;
			}
		}
	} else { /* index: recurse into children */
		ext4_extent_idx *ix = (ext4_extent_idx *)(eh + 1);
		uint8_t *cb = (uint8_t *)kalloc(fs->block_size);
		if (!cb)
			return n; /* OOM: leave subtree intact */
		for (unsigned e = 0; e < n; e++) {
			unsigned long cstart = ix[e].ei_block;
			unsigned long cend =
				(e + 1 < n) ?
					(unsigned long)ix[e + 1].ei_block :
					~0UL;
			unsigned long child =
				(unsigned long)ix[e].ei_leaf_lo |
				((unsigned long)ix[e].ei_leaf_hi << 32);
			(void)cstart;
			if (cend <= from) { /* child entirely kept */
				if (keep != e)
					ix[keep] = ix[e];
				keep++;
				continue;
			}
			if (ext4_read_block(fs, child, cb) != ST_OK) {
				if (keep != e)
					ix[keep] = ix[e];
				keep++;
				continue;
			}
			unsigned sub = ext4_ext_free_subtree(fs, ino, gen, cb,
							     from, freed);
			if (sub == 0) {
				ext4_free_block(fs, child);
				(*freed)++;
			} /* child emptied: drop */
			else {
				ext4_ext_block_csum_set(fs, ino, gen, cb);
				ext4_write_block(fs, child, cb);
				if (keep != e)
					ix[keep] = ix[e];
				keep++;
			}
		}
		kfree(cb);
	}
	eh->eh_entries = (uint16_t)keep;
	return keep;
}

static void ext4_free_blocks_from(ext4_fs_t *fs, unsigned long ino,
				  ext4_inode *in, unsigned long from)
{
	if (!(in->i_flags & EXT4_INODE_EXTENTS_FL))
		return; /* extents only */
	ext4_extent_header *eh = (ext4_extent_header *)in->i_block;
	if (eh->eh_magic != EXT4_EXT_MAGIC)
		return;
	unsigned long freed = 0;
	ext4_ext_free_subtree(fs, ino, in->i_generation, in->i_block, from,
			      &freed);
	if (eh->eh_entries == 0) { /* fully freed: reset to empty leaf root */
		eh->eh_depth = 0;
		eh->eh_max = 4;
	}
	unsigned long fsub = freed * (fs->block_size / 512);
	in->i_blocks_lo =
		(in->i_blocks_lo >= fsub) ? (in->i_blocks_lo - fsub) : 0;
}

/* ===================================================================
 * Extent tree: sorted insertion with node splits.
 *
 * The append path above (ext4_ext_add) covers a file growing at its end.  A
 * write filling a hole inside the file needs its extent placed where it
 * belongs in the tree, and that is what this section does, the way the
 * on-disk format requires it:
 *   - the entries of every node stay sorted by logical block;
 *   - an index entry's key is the first logical block of its child, at
 *     every level (fsck checks the equality, not just the order), so a
 *     change to a node's first entry is carried up as far as it is the
 *     first entry (ext4_ext_correct_index);
 *   - a full node is split in two under a parent with room; a full path
 *     grows the tree by one level at the root (ext4_ext_grow_depth).
 * The new extent is merged into a neighbour it continues on disk and in the
 * file, when both are initialized and the sum still fits one extent.
 *
 * A path holds one buffer per level; level 0 is the inode's inline root
 * (never written here -- the caller stores the inode), levels 1..depth are
 * external blocks, written back through the journal with their checksum
 * tail.  Callers hold the metadata lock exclusively.
 * =================================================================== */

#define EXT4_EXT_DEPTH_MAX 5 /* levels below the root */

struct ext4_ext_path {
	unsigned depth;
	uint8_t *node[EXT4_EXT_DEPTH_MAX + 1];
	unsigned long blk[EXT4_EXT_DEPTH_MAX + 1];
	/* Entry chosen at each level: the last one starting at or before the
	 * block looked up (index levels: the first one when none does);
	 * -1 in a leaf where no extent starts at or before it. */
	int pos[EXT4_EXT_DEPTH_MAX + 1];
};

static inline ext4_extent_header *ext4_ext_hdr(const struct ext4_ext_path *p,
					       unsigned lvl)
{
	return (ext4_extent_header *)p->node[lvl];
}

static inline ext4_extent *ext4_ext_first(const struct ext4_ext_path *p,
					  unsigned lvl)
{
	return (ext4_extent *)(ext4_ext_hdr(p, lvl) + 1);
}

static inline ext4_extent_idx *ext4_ext_first_idx(const struct ext4_ext_path *p,
						  unsigned lvl)
{
	return (ext4_extent_idx *)(ext4_ext_hdr(p, lvl) + 1);
}

/* Blocks an extent maps (an uninitialized one stores its length + 32768). */
static inline unsigned ext4_ext_len(const ext4_extent *e)
{
	return e->ee_len > 32768 ? e->ee_len - 32768u : e->ee_len;
}

static inline unsigned long ext4_ext_pblk(const ext4_extent *e)
{
	return (unsigned long)e->ee_start_lo |
	       ((unsigned long)e->ee_start_hi << 32);
}

static inline unsigned long ext4_idx_pblk(const ext4_extent_idx *ix)
{
	return (unsigned long)ix->ei_leaf_lo |
	       ((unsigned long)ix->ei_leaf_hi << 32);
}

/* Store an initialized extent. */
static void ext4_ext_store(ext4_extent *e, unsigned long lblk,
			   unsigned long pblk, unsigned len)
{
	BUG_ON(len == 0 || len > 32768);
	e->ee_block = (uint32_t)lblk;
	e->ee_len = (uint16_t)len;
	e->ee_start_hi = (uint16_t)(pblk >> 32);
	e->ee_start_lo = (uint32_t)pblk;
}

/* Can `b' be folded into `a' (a immediately before b, in the file and on
 * the device, both initialized, and the sum still fits one extent)? */
static int ext4_ext_mergeable(const ext4_extent *a, const ext4_extent *b)
{
	unsigned la = ext4_ext_len(a), lb = ext4_ext_len(b);

	if (a->ee_len > 32768 || b->ee_len > 32768)
		return 0;
	if ((unsigned long)a->ee_block + la != b->ee_block)
		return 0;
	if (ext4_ext_pblk(a) + la != ext4_ext_pblk(b))
		return 0;
	return la + lb <= 32768;
}

static void ext4_ext_path_free(struct ext4_ext_path *p)
{
	for (unsigned l = 1; l <= p->depth && l <= EXT4_EXT_DEPTH_MAX; l++)
		if (p->node[l]) {
			kfree(p->node[l]);
			p->node[l] = 0;
		}
}

/* Is a node header sane for its place in the tree? */
static int ext4_ext_hdr_ok(const ext4_fs_t *fs, const ext4_extent_header *h,
			   unsigned want_depth, int is_root)
{
	unsigned max = is_root ? 4 : ext4_ext_extern_max(fs);

	/* An index node (depth > 0) with no entries points nowhere. */
	return h->eh_magic == EXT4_EXT_MAGIC && h->eh_depth == want_depth &&
	       h->eh_max != 0 && h->eh_max <= max &&
	       h->eh_entries <= h->eh_max &&
	       !(h->eh_entries == 0 && want_depth > 0);
}

/* Look `lblk' up: fill `p' with the path from the root to the leaf that
 * would hold it.  ST_OK, or ST_IO for a damaged tree (reported). */
static int ext4_ext_find(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			 unsigned long lblk, struct ext4_ext_path *p)
{
	ext4_extent_header *root = (ext4_extent_header *)in->i_block;

	mm_memset(p, 0, sizeof(*p));
	p->node[0] = in->i_block;
	if (root->eh_magic != EXT4_EXT_MAGIC ||
	    root->eh_depth > EXT4_EXT_DEPTH_MAX ||
	    !ext4_ext_hdr_ok(fs, root, root->eh_depth, 1)) {
		ext4_fs_error(fs, "bad extent tree root", ino);
		return ST_IO;
	}
	p->depth = root->eh_depth;
	for (unsigned lvl = 0;; lvl++) {
		ext4_extent_header *h = ext4_ext_hdr(p, lvl);
		unsigned n = h->eh_entries;
		int pos = -1;

		if (lvl == p->depth) {
			ext4_extent *ex = (ext4_extent *)(h + 1);

			for (unsigned i = 0; i < n && ex[i].ee_block <= lblk;
			     i++)
				pos = (int)i;
			p->pos[lvl] = pos;
			return ST_OK;
		}
		ext4_extent_idx *ix = (ext4_extent_idx *)(h + 1);

		if (n == 0) {
			ext4_fs_error(fs, "empty extent index node", ino);
			ext4_ext_path_free(p);
			return ST_IO;
		}
		pos = 0;
		for (unsigned i = 1; i < n && ix[i].ei_block <= lblk; i++)
			pos = (int)i;
		p->pos[lvl] = pos;

		unsigned long child = ext4_idx_pblk(&ix[pos]);
		uint8_t *buf = (uint8_t *)kalloc(fs->block_size);

		if (!buf) {
			ext4_ext_path_free(p);
			return ST_NOMEM;
		}
		p->node[lvl + 1] = buf;
		p->blk[lvl + 1] = child;
		if (ext4_read_block(fs, child, buf) != ST_OK) {
			ext4_ext_path_free(p);
			return ST_IO;
		}
		if (!ext4_mbc_verified(fs, child)) {
			if (!ext4_extent_block_csum_ok(fs, ino, in->i_generation,
						       buf)) {
				ext4_fs_error(fs,
					      "extent block checksum mismatch",
					      ino);
				ext4_ext_path_free(p);
				return ST_IO;
			}
			ext4_mbc_mark_verified(fs, child);
		}
		if (!ext4_ext_hdr_ok(fs, (ext4_extent_header *)buf,
				     p->depth - lvl - 1, 0)) {
			ext4_fs_error(fs, "bad extent tree node", ino);
			ext4_ext_path_free(p);
			return ST_IO;
		}
	}
}

/* Store the node at `lvl' (the inline root is stored with the inode). */
static int ext4_ext_dirty(ext4_fs_t *fs, unsigned long ino,
			  const ext4_inode *in, struct ext4_ext_path *p,
			  unsigned lvl)
{
	if (lvl == 0)
		return ST_OK;
	ext4_ext_block_csum_set(fs, ino, in->i_generation, p->node[lvl]);
	return ext4_write_block(fs, p->blk[lvl], p->node[lvl]);
}

static uint32_t ext4_ext_node_key(const struct ext4_ext_path *p, unsigned lvl)
{
	if (lvl == p->depth)
		return ext4_ext_first(p, lvl)[0].ee_block;
	return ext4_ext_first_idx(p, lvl)[0].ei_block;
}

/* The first entry of the node at `lvl' changed: give its index entry the
 * new key, and the same one level up for as long as the entry changed is
 * the first of its node. */
static void ext4_ext_correct_index(ext4_fs_t *fs, unsigned long ino,
				   const ext4_inode *in,
				   struct ext4_ext_path *p, unsigned lvl)
{
	if (ext4_ext_hdr(p, lvl)->eh_entries == 0)
		return;
	uint32_t key = ext4_ext_node_key(p, lvl);

	for (unsigned l = lvl; l > 0; l--) {
		ext4_extent_idx *ix = &ext4_ext_first_idx(p, l - 1)[p->pos[l - 1]];

		if (ix->ei_block == key)
			break;
		ix->ei_block = key;
		ext4_ext_dirty(fs, ino, in, p, l - 1);
		if (p->pos[l - 1] != 0)
			break;
	}
}

/* Make room in the leaf of `p' for one more entry, without inserting it:
 * split the deepest full node whose parent has room (moving its upper half
 * to a new block), or, when every node on the path is full, add a level at
 * the root.  One step per call; the caller looks the block up again and
 * repeats until the leaf has room.  Frees the path. */
static int ext4_ext_make_room(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			      struct ext4_ext_path *p)
{
	int parent = -1;

	for (int l = (int)p->depth - 1; l >= 0; l--)
		if (ext4_ext_hdr(p, (unsigned)l)->eh_entries <
		    ext4_ext_hdr(p, (unsigned)l)->eh_max) {
			parent = l;
			break;
		}
	if (parent < 0) {
		ext4_ext_path_free(p);
		if (((ext4_extent_header *)in->i_block)->eh_depth >=
		    EXT4_EXT_DEPTH_MAX)
			return ST_NOSPC;
		return ext4_ext_grow_depth(fs, ino, in);
	}

	unsigned lvl = (unsigned)parent + 1;
	ext4_extent_header *h = ext4_ext_hdr(p, lvl);
	unsigned n = h->eh_entries, half = n / 2;
	unsigned long nb;

	/* A full node of fewer than two entries cannot be split at its middle.
	 * The driver never writes one (an external node gets the block's whole
	 * capacity), but a node read from the disk may carry any eh_max from 1
	 * up (ext4_ext_hdr_ok accepts what the format allows): damaged or
	 * foreign metadata is an error to report, not a kernel bug. */
	if (n < 2) {
		ext4_ext_path_free(p);
		ext4_fs_error(fs, "extent node too small to split", ino);
		return ST_IO;
	}
	nb = ext4_alloc_one_block(fs);
	if (!nb) {
		ext4_ext_path_free(p);
		return ST_NOSPC;
	}
	uint8_t *buf = (uint8_t *)kalloc(fs->block_size);

	if (!buf) {
		ext4_free_block(fs, nb);
		ext4_ext_path_free(p);
		return ST_NOMEM;
	}
	mm_memset(buf, 0, fs->block_size);
	ext4_extent_header *nh = (ext4_extent_header *)buf;

	nh->eh_magic = EXT4_EXT_MAGIC;
	nh->eh_depth = h->eh_depth;
	nh->eh_max = (uint16_t)ext4_ext_extern_max(fs);
	nh->eh_entries = (uint16_t)(n - half);
	/* Index entries and extents are both 12 bytes. */
	mm_memcpy(nh + 1, (uint8_t *)(h + 1) + (unsigned long)half * 12,
		  (unsigned long)(n - half) * 12);
	uint32_t key = ((ext4_extent_idx *)(nh + 1))->ei_block;

	ext4_ext_block_csum_set(fs, ino, in->i_generation, buf);
	int rc = ext4_write_block(fs, nb, buf);

	kfree(buf);
	if (rc != ST_OK) {
		ext4_free_block(fs, nb);
		ext4_ext_path_free(p);
		return rc;
	}
	h->eh_entries = (uint16_t)half;
	mm_memset((uint8_t *)(h + 1) + (unsigned long)half * 12, 0,
		  (unsigned long)(n - half) * 12);
	ext4_ext_dirty(fs, ino, in, p, lvl);

	/* The new node's entry goes right after the split node's. */
	ext4_extent_header *ph = ext4_ext_hdr(p, (unsigned)parent);
	ext4_extent_idx *pix = ext4_ext_first_idx(p, (unsigned)parent);
	unsigned at = (unsigned)p->pos[parent] + 1;

	BUG_ON(ph->eh_entries >= ph->eh_max || at > ph->eh_entries);
	for (unsigned i = ph->eh_entries; i > at; i--)
		pix[i] = pix[i - 1];
	pix[at].ei_block = key;
	pix[at].ei_leaf_lo = (uint32_t)nb;
	pix[at].ei_leaf_hi = (uint16_t)(nb >> 32);
	pix[at].ei_unused = 0;
	ph->eh_entries++;
	ext4_ext_dirty(fs, ino, in, p, (unsigned)parent);
	in->i_blocks_lo += fs->block_size / 512; /* nb is a metadata block */
	ext4_ext_path_free(p);
	return ST_OK;
}

/* Insert the initialized extent [lblk, lblk+len) -> pblk (one extent's
 * worth at most, not overlapping anything mapped), folded into a neighbour
 * it continues where it can.  ST_OK / ST_NOSPC / ST_NOMEM / ST_IO; on
 * failure nothing was inserted. */
static int ext4_ext_insert(ext4_fs_t *fs, unsigned long ino, ext4_inode *in,
			   unsigned long lblk, unsigned long pblk, unsigned len)
{
	struct ext4_ext_path p;
	ext4_extent ne;

	ext4_ext_store(&ne, lblk, pblk, len);
	for (unsigned tries = 0; tries < 4 * (EXT4_EXT_DEPTH_MAX + 2);
	     tries++) {
		int rc = ext4_ext_find(fs, ino, in, lblk, &p);

		if (rc != ST_OK)
			return rc;
		unsigned d = p.depth;
		ext4_extent_header *h = ext4_ext_hdr(&p, d);
		ext4_extent *ex = ext4_ext_first(&p, d);
		int at = p.pos[d];
		unsigned n = h->eh_entries;

		if ((at >= 0 && (unsigned long)ex[at].ee_block +
						ext4_ext_len(&ex[at]) >
					lblk) ||
		    (at + 1 < (int)n &&
		     ex[at + 1].ee_block < lblk + (unsigned long)len)) {
			WARN(1, "ext4: inode %lu: extent %lu+%u overlaps the tree",
			     ino, lblk, len);
			ext4_ext_path_free(&p);
			return ST_IO;
		}
		if (at >= 0 && ext4_ext_mergeable(&ex[at], &ne)) {
			unsigned nl = ext4_ext_len(&ex[at]) + len;

			ext4_ext_store(&ex[at], ex[at].ee_block,
				       ext4_ext_pblk(&ex[at]), nl);
			if (at + 1 < (int)n &&
			    ext4_ext_mergeable(&ex[at], &ex[at + 1])) {
				nl += ext4_ext_len(&ex[at + 1]);
				ext4_ext_store(&ex[at], ex[at].ee_block,
					       ext4_ext_pblk(&ex[at]), nl);
				for (unsigned i = (unsigned)at + 1; i + 1 < n;
				     i++)
					ex[i] = ex[i + 1];
				h->eh_entries--;
				mm_memset(&ex[h->eh_entries], 0, sizeof(*ex));
			}
			rc = ext4_ext_dirty(fs, ino, in, &p, d);
			ext4_ext_path_free(&p);
			return rc;
		}
		if (at + 1 < (int)n && ext4_ext_mergeable(&ne, &ex[at + 1])) {
			ext4_ext_store(&ex[at + 1], lblk, pblk,
				       len + ext4_ext_len(&ex[at + 1]));
			rc = ext4_ext_dirty(fs, ino, in, &p, d);
			if (at + 1 == 0)
				ext4_ext_correct_index(fs, ino, in, &p, d);
			ext4_ext_path_free(&p);
			return rc;
		}
		if (n < h->eh_max) {
			unsigned i0 = (unsigned)(at + 1);

			for (unsigned i = n; i > i0; i--)
				ex[i] = ex[i - 1];
			ex[i0] = ne;
			h->eh_entries++;
			rc = ext4_ext_dirty(fs, ino, in, &p, d);
			if (i0 == 0)
				ext4_ext_correct_index(fs, ino, in, &p, d);
			ext4_ext_path_free(&p);
			return rc;
		}
		rc = ext4_ext_make_room(fs, ino, in, &p);
		if (rc != ST_OK)
			return rc;
	}
	WARN(1, "ext4: inode %lu: no room for an extent after splitting",
	     ino);
	return ST_IO;
}

/* Map the blocks [lblk, lblk+len) -> pblk.. as initialized extents,
 * extent-sized piece by piece.  Returns the blocks mapped. */
static unsigned ext4_ext_insert_run(ext4_fs_t *fs, unsigned long ino,
				    ext4_inode *in, unsigned long lblk,
				    unsigned long pblk, unsigned len)
{
	unsigned done = 0;

	while (done < len) {
		unsigned c = len - done > 32768 ? 32768 : len - done;

		if (ext4_ext_insert(fs, ino, in, lblk + done, pblk + done, c) !=
		    ST_OK)
			break;
		done += c;
	}
	return done;
}

/* ===================================================================
 * Extended attributes (xattr) — storage layer.
 *
 * In-inode ("ibody") attributes — those stored in the inode's
 * slack space [128 + i_extra_isize, s_inode_size).  This region is covered by
 * the inode metadata_csum (already computed correctly), so there is no separate
 * on-disk hash/checksum to get byte-exact here.  The external xattr block (with
 * its own hash + crc32c) is a later increment; get/list already leave a hook for
 * it.  Values stored in a separate inode (e_value_inum != 0) are not produced by
 * us and are treated as absent on read.
 * =================================================================== */

/* xattr set flags (match the userspace XATTR_CREATE / XATTR_REPLACE). */
#define EXT4_XATTR_CREATE 1 /* fail with EEXIST if the attr exists      */
#define EXT4_XATTR_REPLACE 2 /* fail with ENODATA if it does not exist   */

/* Map a namespace prefix to its e_name_index; *suffix and *slen receive the
 * part after the prefix.  Returns 0 for an unknown/unsupported namespace. */
static int ext4_xattr_name_index(const char *full, const char **suffix,
				 unsigned *slen)
{
	static const struct {
		const char *p;
		int idx;
	} pre[] = {
		{ "user.", EXT4_XATTR_INDEX_USER },
		{ "system.posix_acl_access",
		  EXT4_XATTR_INDEX_POSIX_ACL_ACCESS },
		{ "system.posix_acl_default",
		  EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT },
		{ "trusted.", EXT4_XATTR_INDEX_TRUSTED },
		{ "security.", EXT4_XATTR_INDEX_SECURITY },
		{ "system.", EXT4_XATTR_INDEX_SYSTEM },
	};
	for (unsigned k = 0; k < sizeof(pre) / sizeof(pre[0]); k++) {
		const char *p = pre[k].p;
		const char *f = full;
		while (*p && *p == *f) {
			p++;
			f++;
		}
		if (*p)
			continue; /* prefix didn't fully match */
		/* posix_acl_* are whole names with an EMPTY on-disk name (the index is
         * the identity); the others are prefixes that require a suffix. */
		int whole = (pre[k].idx == EXT4_XATTR_INDEX_POSIX_ACL_ACCESS ||
			     pre[k].idx == EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT);
		if (whole && *f)
			continue; /* whole name must match exactly */
		if (!whole && !*f)
			continue; /* prefix name needs a suffix    */
		*suffix = f;
		unsigned n = 0;
		while (f[n])
			n++;
		*slen = n;
		return pre[k].idx;
	}
	return 0;
}

/* The namespace prefix for an index (for listxattr output).  NULL if unknown. */
static const char *ext4_xattr_prefix(int idx)
{
	switch (idx) {
	case EXT4_XATTR_INDEX_USER:
		return "user.";
	case EXT4_XATTR_INDEX_POSIX_ACL_ACCESS:
		return "system.posix_acl_access";
	case EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT:
		return "system.posix_acl_default";
	case EXT4_XATTR_INDEX_TRUSTED:
		return "trusted.";
	case EXT4_XATTR_INDEX_SECURITY:
		return "security.";
	case EXT4_XATTR_INDEX_SYSTEM:
		return "system.";
	default:
		return 0;
	}
}

/* In-memory attribute used while rebuilding a region (parse -> modify -> serialize). */
#define EXT4_XA_MAX_ATTRS 32
#define EXT4_XA_MAX_NAME 255
typedef struct {
	uint8_t index;
	uint8_t name_len;
	char name[EXT4_XA_MAX_NAME];
	uint32_t value_size;
	uint8_t *value; /* kalloc'd (value_size bytes) or NULL    */
} ext4_xa_attr;
typedef struct {
	ext4_xa_attr a[EXT4_XA_MAX_ATTRS];
	unsigned n;
} ext4_xa_list;

static void ext4_xa_list_free(ext4_xa_list *L)
{
	for (unsigned i = 0; i < L->n; i++)
		if (L->a[i].value) {
			kfree(L->a[i].value);
			L->a[i].value = 0;
		}
	L->n = 0;
}

/* Parse the entries of one xattr region into L.  `first` points at the first
 * entry, `end` bounds both entries and (inline) values, `value_base` is what
 * e_value_offs is measured from (the block start for a block; `first` for an
 * ibody region). */
static int ext4_xa_parse(ext4_xa_list *L, const uint8_t *first,
			 const uint8_t *end, const uint8_t *value_base)
{
	const uint8_t *p = first;
	while (p + sizeof(ext4_xattr_entry) <= end) {
		const ext4_xattr_entry *e = (const ext4_xattr_entry *)p;
		uint32_t first4;
		mm_memcpy(&first4, p, 4);
		if (first4 == 0)
			break; /* zero terminator (name_len &
                                                   * name_index both 0).  A POSIX
                                                   * ACL entry has name_len==0 but
                                                   * name_index!=0, so first4!=0. */
		const uint8_t *nm = p + sizeof(ext4_xattr_entry);
		if (nm + e->e_name_len > end)
			break; /* malformed: stop          */
		if (L->n <
		    EXT4_XA_MAX_ATTRS) { /* e_name_len is u8 <= EXT4_XA_MAX_NAME */
			ext4_xa_attr *a = &L->a[L->n];
			a->index = e->e_name_index;
			a->name_len = e->e_name_len;
			mm_memcpy(a->name, nm, e->e_name_len);
			a->value_size = e->e_value_size;
			a->value = 0;
			if (e->e_value_inum == 0 && e->e_value_size > 0) {
				const uint8_t *v = value_base + e->e_value_offs;
				if (v >= first && v + e->e_value_size <= end) {
					a->value = (uint8_t *)kalloc(
						e->e_value_size);
					if (a->value)
						mm_memcpy(a->value, v,
							  e->e_value_size);
				}
			}
			L->n++;
		}
		p = nm +
		    ((e->e_name_len + EXT4_XATTR_ROUND) & ~EXT4_XATTR_ROUND);
	}
	return ST_OK;
}

/* Bytes a list needs when serialized into a region (header + entries + 4-byte
 * terminator + 4-byte-aligned values).  `hdr_size` is 4 for ibody, 32 for a
 * block. */
static unsigned long ext4_xa_serial_size(const ext4_xa_list *L,
					 unsigned hdr_size)
{
	unsigned long ent = 0, val = 0;
	for (unsigned i = 0; i < L->n; i++) {
		ent += EXT4_XATTR_LEN(L->a[i].name_len);
		val += EXT4_XATTR_SIZE(L->a[i].value_size);
	}
	return hdr_size + ent + 4 /*terminator*/ + val;
}

/* Find an attribute in L by (index,name); returns its index in L or -1. */
static int ext4_xa_find(const ext4_xa_list *L, int index, const char *name,
			unsigned nlen)
{
	for (unsigned i = 0; i < L->n; i++) {
		if (L->a[i].index != index || L->a[i].name_len != nlen)
			continue;
		unsigned k = 0;
		while (k < nlen && L->a[i].name[k] == name[k])
			k++;
		if (k == nlen)
			return (int)i;
	}
	return -1;
}

/* Serialize L into an ibody region [region, region+size).  Entries grow forward
 * after the 4-byte ibody header; values grow backward from region+size.  Returns
 * ST_OK, or ST_NOSPC if it does not fit.  e_hash is left 0 (ibody entries are not
 * part of any shareable-block hash; the inode csum covers the region). */
static int ext4_xa_serialize_ibody(uint8_t *region, unsigned long size,
				   const ext4_xa_list *L)
{
	if (L->n == 0) { /* no attrs: clear the region */
		mm_memset(region, 0, size);
		return ST_OK;
	}
	if (ext4_xa_serial_size(L, sizeof(ext4_xattr_ibody_header)) > size)
		return ST_NOSPC;
	mm_memset(region, 0, size);
	ext4_xattr_ibody_header *h = (ext4_xattr_ibody_header *)region;
	h->h_magic = EXT4_XATTR_MAGIC;
	uint8_t *ifirst = region + sizeof(ext4_xattr_ibody_header);
	uint8_t *ep = ifirst;
	unsigned long vpos = size; /* value offsets are from ifirst */
	for (unsigned i = 0; i < L->n; i++) {
		const ext4_xa_attr *a = &L->a[i];
		unsigned vlen = EXT4_XATTR_SIZE(a->value_size);
		vpos -= vlen; /* place value (region-relative) */
		if (a->value_size && a->value)
			mm_memcpy(region + vpos, a->value, a->value_size);
		ext4_xattr_entry *e = (ext4_xattr_entry *)ep;
		e->e_name_len = a->name_len;
		e->e_name_index = a->index;
		e->e_value_offs =
			(uint16_t)(vpos -
				   (unsigned long)(ifirst -
						   region)); /* from ifirst */
		e->e_value_inum = 0;
		e->e_value_size = a->value_size;
		e->e_hash = 0;
		mm_memcpy(e->e_name, a->name, a->name_len);
		ep += EXT4_XATTR_LEN(a->name_len);
	}
	return ST_OK;
}

/* ---- external xattr block (i_file_acl) ----  Large attributes that
 * don't fit the inode slack live in one shared-format block: a 32-byte header
 * then forward-growing entries and backward-growing values, with per-entry and
 * whole-block hashes (legacy, always present) plus a crc32c (metadata_csum).
 * All three are validated by e2fsck, so they must be byte-exact. */

/* Allocate one block from the bitmaps, not attached to any inode (for the xattr
 * block).  Returns the physical block number or 0.  Caller adjusts i_blocks. */
static unsigned long ext4_alloc_one_block(ext4_fs_t *fs)
{
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	if (!bm)
		return 0;
	unsigned long pbn = 0;
	for (unsigned g = 0; g < fs->groups_count && !pbn; g++) {
		if (fs->gdt[g].bg_free_blocks_count_lo == 0)
			continue;
		/* Group lock: this group's block-bitmap read-modify-write. */
		ext4_bg_lock(fs, g);
		unsigned long bblk = ext4_gd_block_bitmap(fs, g);
		int uninit = 0;
		int lst = ext4_load_block_bitmap(fs, g, bm, &uninit);
		if (lst == EXT4_BM_CORRUPT) {
			ext4_bg_unlock(fs, g);
			break;
		}
		if (lst != ST_OK) {
			ext4_bg_unlock(fs, g);
			continue;
		}
		unsigned long base = fs->first_data_block +
				     (unsigned long)g * fs->blocks_per_group;
		for (unsigned i = 0; i < fs->blocks_per_group; i++) {
			if (ext4_bm_test(bm, i))
				continue;
			if (uninit)
				ext4_bg_block_uninit_clear(
					fs, g, ext4_bitmap_free_blocks(fs, g, bm));
			ext4_bm_set(bm, i);
			ext4_block_bitmap_csum_set(fs, g, bm);
			ext4_write_block(fs, bblk, bm);
			fs->gdt[g].bg_free_blocks_count_lo--;
			fs->sb_copy.s_free_blocks_count_lo--;
			ext4_gd_dirty(fs, g);
			pbn = base + i;
			break;
		}
		ext4_bg_unlock(fs, g);
	}
	kfree(bm);
	return pbn;
}

/* Per-entry xattr hash: fold the name then the (4-byte-padded, inline) value.
 * `block_base` is the start the entry's e_value_offs is measured from. */
static uint32_t ext4_xattr_entry_hash(const ext4_xattr_entry *e,
				      const uint8_t *block_base)
{
	uint32_t hash = 0;
	const uint8_t *name = (const uint8_t *)e->e_name;
	for (unsigned n = 0; n < e->e_name_len; n++)
		hash = (hash << 5) ^ (hash >> (32 - 5)) ^
		       name[n]; /* NAME_HASH_SHIFT=5 */
	if (e->e_value_inum == 0 && e->e_value_size) {
		const uint8_t *v = block_base + e->e_value_offs;
		unsigned words = (e->e_value_size + EXT4_XATTR_ROUND) >>
				 2; /* padded /4 */
		for (unsigned n = 0; n < words; n++) {
			uint32_t w;
			mm_memcpy(&w, v + (unsigned long)n * 4, 4);
			hash = (hash << 16) ^ (hash >> (32 - 16)) ^
			       w; /* VALUE_HASH_SHIFT=16 */
		}
	}
	return hash;
}

/* Whole-block hash: fold every entry's e_hash (0 if any entry hash is 0, which
 * marks the block non-shareable — matches jbd2/ext4 ext4_xattr_rehash). */
static void ext4_xattr_rehash(ext4_xattr_header *h, const uint8_t *block_base)
{
	uint32_t hash = 0;
	const uint8_t *p = block_base + sizeof(ext4_xattr_header);
	for (;;) {
		const ext4_xattr_entry *e = (const ext4_xattr_entry *)p;
		uint32_t first4;
		mm_memcpy(&first4, p, 4);
		if (first4 == 0)
			break; /* terminator */
		if (e->e_hash == 0) {
			hash = 0;
			break;
		}
		hash = (hash << 16) ^ (hash >> (32 - 16)) ^
		       e->e_hash; /* BLOCK_HASH_SHIFT=16 */
		p += EXT4_XATTR_LEN(e->e_name_len);
	}
	h->h_hash = hash;
}

/* crc32c of an xattr block (metadata_csum): seed with the fs csum seed folded
 * with the LE block number, then the block with h_checksum treated as zero. */
static uint32_t ext4_xattr_block_csum(const ext4_fs_t *fs,
				      unsigned long blocknr,
				      const uint8_t *block)
{
	uint64_t dsk = (uint64_t)blocknr; /* little-endian on x86-64 */
	uint32_t dummy = 0;
	unsigned coff = __builtin_offsetof(ext4_xattr_header, h_checksum);
	uint32_t c = ext4_crc32c(fs->csum_seed, &dsk, sizeof(dsk));
	c = ext4_crc32c(c, block, coff); /* header up to h_checksum   */
	c = ext4_crc32c(c, &dummy,
			sizeof(dummy)); /* the zeroed h_checksum     */
	c = ext4_crc32c(c, block + coff + sizeof(dummy),
			fs->block_size - (coff + sizeof(dummy)));
	return c;
}
static void ext4_xattr_block_csum_set(const ext4_fs_t *fs,
				      unsigned long blocknr, uint8_t *block)
{
	ext4_xattr_header *h = (ext4_xattr_header *)block;
	h->h_checksum = fs->has_metadata_csum ?
				ext4_xattr_block_csum(fs, blocknr, block) :
				0;
}
static int ext4_xattr_block_csum_ok(const ext4_fs_t *fs, unsigned long blocknr,
				    const uint8_t *block)
{
	if (!fs->has_metadata_csum)
		return 1;
	const ext4_xattr_header *h = (const ext4_xattr_header *)block;
	return h->h_checksum == ext4_xattr_block_csum(fs, blocknr, block);
}

/* Serialize L into a fresh external xattr block.  Entries grow forward after the
 * 32-byte header; values grow backward from the block end (e_value_offs is from
 * the block start).  Stamps per-entry hashes, the block hash, then the crc32c.
 * Returns ST_OK, or ST_NOSPC if it does not fit one block. */
static int ext4_xa_serialize_block(ext4_fs_t *fs, uint8_t *block,
				   unsigned long blocknr, const ext4_xa_list *L)
{
	if (ext4_xa_serial_size(L, sizeof(ext4_xattr_header)) > fs->block_size)
		return ST_NOSPC;
	mm_memset(block, 0, fs->block_size);
	ext4_xattr_header *h = (ext4_xattr_header *)block;
	h->h_magic = EXT4_XATTR_MAGIC;
	h->h_refcount = 1;
	h->h_blocks = 1;
	uint8_t *ep = block + sizeof(ext4_xattr_header);
	unsigned long vpos = fs->block_size;
	for (unsigned i = 0; i < L->n; i++) {
		const ext4_xa_attr *a = &L->a[i];
		unsigned vlen = EXT4_XATTR_SIZE(a->value_size);
		vpos -= vlen;
		if (a->value_size && a->value)
			mm_memcpy(block + vpos, a->value, a->value_size);
		ext4_xattr_entry *e = (ext4_xattr_entry *)ep;
		e->e_name_len = a->name_len;
		e->e_name_index = a->index;
		e->e_value_offs = (uint16_t)vpos; /* block-relative */
		e->e_value_inum = 0;
		e->e_value_size = a->value_size;
		e->e_hash = 0;
		mm_memcpy(e->e_name, a->name, a->name_len);
		ep += EXT4_XATTR_LEN(a->name_len);
	}
	/* hashes must be computed after the values are in place */
	ep = block + sizeof(ext4_xattr_header);
	for (unsigned i = 0; i < L->n; i++) {
		ext4_xattr_entry *e = (ext4_xattr_entry *)ep;
		e->e_hash = ext4_xattr_entry_hash(e, block);
		ep += EXT4_XATTR_LEN(e->e_name_len);
	}
	ext4_xattr_rehash(h, block);
	ext4_xattr_block_csum_set(fs, blocknr, block);
	return ST_OK;
}

/* Locate the ibody xattr region of inode `ino` within its inode-table block.
 * Returns ST_OK with *blk and *off (the block + inode offset), *rstart (offset of
 * the region within the block) and *rsize (region length).  *rsize==0 if the
 * inode has no usable ibody xattr space. */
static int ext4_xattr_ibody_region(ext4_fs_t *fs, unsigned long ino,
				   ext4_inode *in, unsigned long *blk,
				   unsigned *off, unsigned *rstart,
				   unsigned *rsize)
{
	int st = ext4_read_inode_loc(fs, ino, in, blk, off);
	if (st != ST_OK)
		return st;
	unsigned extra = in->i_extra_isize;
	unsigned start = *off + EXT4_GOOD_OLD_INODE_SIZE + extra;
	unsigned endb = *off + fs->inode_size;
	if (fs->inode_size <= EXT4_GOOD_OLD_INODE_SIZE || extra < 4 ||
	    start + sizeof(ext4_xattr_ibody_header) >= endb) {
		*rstart = 0;
		*rsize = 0;
	} else {
		*rstart = start;
		*rsize = endb - start;
	}
	return ST_OK;
}

/* getxattr: copy the value of (index,name) into buf (or return its size if
 * buf==NULL / size==0).  Returns the value size, ST_NOT_FOUND, or ST_INVALID
 * (buffer too small => -ERANGE handled by the caller). */
static int ext4_xattr_get_ino(ext4_fs_t *fs, unsigned long ino, int index,
			      const char *name, unsigned nlen, void *buf,
			      unsigned long bufsz)
{
	ext4_inode in;
	unsigned long blk;
	unsigned off, rstart, rsize;
	int st = ext4_xattr_ibody_region(fs, ino, &in, &blk, &off, &rstart,
					 &rsize);
	if (st != ST_OK)
		return st;
	int found_size = ST_NODATA;
	if (rsize) {
		uint8_t *bbuf = (uint8_t *)ext4_bget(fs);
		if (!bbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, blk, bbuf) == ST_OK) {
			uint8_t *region = bbuf + rstart;
			uint32_t mag;
			mm_memcpy(&mag, region, 4);
			if (mag == EXT4_XATTR_MAGIC) {
				uint8_t *ifirst =
					region +
					sizeof(ext4_xattr_ibody_header);
				uint8_t *rend = bbuf + off + fs->inode_size;
				static ext4_xa_list L;
				ext4_rwsem_write_lock(&s_xa_scratch);
				L.n = 0;
				ext4_xa_parse(&L, ifirst, rend, ifirst);
				int i = ext4_xa_find(&L, index, name, nlen);
				if (i >= 0) {
					unsigned vs = L.a[i].value_size;
					if (bufsz == 0 || buf == 0)
						found_size = (int)vs;
					else if (vs > bufsz)
						found_size =
							ST_RANGE; /* ERANGE */
					else {
						if (vs && L.a[i].value)
							mm_memcpy(buf,
								  L.a[i].value,
								  vs);
						found_size = (int)vs;
					}
				}
				ext4_xa_list_free(&L);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		ext4_bput(fs, bbuf);
	}
	/* External xattr block (i_file_acl) — checked only if not found in the ibody. */
	if (found_size == ST_NODATA && in.i_file_acl_lo) {
		uint8_t *xbuf = (uint8_t *)ext4_bget(fs);
		if (!xbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, in.i_file_acl_lo, xbuf) == ST_OK) {
			ext4_xattr_header *xh = (ext4_xattr_header *)xbuf;
			if (xh->h_magic == EXT4_XATTR_MAGIC) {
				if (!ext4_mbc_verified(fs, in.i_file_acl_lo)) {
					if (!ext4_xattr_block_csum_ok(
						    fs, in.i_file_acl_lo,
						    xbuf)) {
						ext4_bput(fs, xbuf);
						ext4_fs_error(
							fs,
							"xattr block checksum mismatch",
							ino);
						return ST_IO;
					}
					ext4_mbc_mark_verified(fs, 
						in.i_file_acl_lo);
				}
				static ext4_xa_list LB;
				ext4_rwsem_write_lock(&s_xa_scratch);
				LB.n = 0;
				ext4_xa_parse(&LB,
					      xbuf + sizeof(ext4_xattr_header),
					      xbuf + fs->block_size, xbuf);
				int i = ext4_xa_find(&LB, index, name, nlen);
				if (i >= 0) {
					unsigned vs = LB.a[i].value_size;
					if (bufsz == 0 || buf == 0)
						found_size = (int)vs;
					else if (vs > bufsz)
						found_size = ST_RANGE;
					else {
						if (vs && LB.a[i].value)
							mm_memcpy(buf,
								  LB.a[i].value,
								  vs);
						found_size = (int)vs;
					}
				}
				ext4_xa_list_free(&LB);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		ext4_bput(fs, xbuf);
	}
	return found_size;
}

/* Append each attribute's full name (prefix+name, NUL-terminated) from L to buf
 * (when non-NULL), tracking the running byte total and a range-overflow flag. */
static void ext4_xa_emit_names(const ext4_xa_list *L, char *buf,
			       unsigned long bufsz, unsigned long *total,
			       int *erange)
{
	for (unsigned i = 0; i < L->n; i++) {
		const char *pre = ext4_xattr_prefix(L->a[i].index);
		if (!pre)
			continue;
		unsigned plen = 0;
		while (pre[plen])
			plen++;
		unsigned full = plen + L->a[i].name_len + 1; /* + NUL */
		if (buf && bufsz) {
			if (*total + full > bufsz) {
				*erange = 1;
			} else {
				mm_memcpy(buf + *total, pre, plen);
				mm_memcpy(buf + *total + plen, L->a[i].name,
					  L->a[i].name_len);
				buf[*total + plen + L->a[i].name_len] = '\0';
			}
		}
		*total += full;
	}
}

/* listxattr: write NUL-separated full names from BOTH the ibody and the external
 * block into buf (or return total size if buf==NULL/size==0).  Returns total
 * bytes, or ST_RANGE (=> -ERANGE). */
static int ext4_xattr_list_ino(ext4_fs_t *fs, unsigned long ino, char *buf,
			       unsigned long bufsz)
{
	ext4_inode in;
	unsigned long blk;
	unsigned off, rstart, rsize;
	int st = ext4_xattr_ibody_region(fs, ino, &in, &blk, &off, &rstart,
					 &rsize);
	if (st != ST_OK)
		return st;
	unsigned long total = 0;
	int erange = 0;
	if (rsize) {
		uint8_t *bbuf = (uint8_t *)kalloc(fs->block_size);
		if (!bbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, blk, bbuf) == ST_OK) {
			uint8_t *region = bbuf + rstart;
			uint32_t mag;
			mm_memcpy(&mag, region, 4);
			if (mag == EXT4_XATTR_MAGIC) {
				uint8_t *ifirst =
					region +
					sizeof(ext4_xattr_ibody_header);
				static ext4_xa_list L;
				ext4_rwsem_write_lock(&s_xa_scratch);
				L.n = 0;
				ext4_xa_parse(&L, ifirst,
					      bbuf + off + fs->inode_size,
					      ifirst);
				ext4_xa_emit_names(&L, buf, bufsz, &total,
						   &erange);
				ext4_xa_list_free(&L);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		kfree(bbuf);
	}
	if (in.i_file_acl_lo) {
		uint8_t *xbuf = (uint8_t *)kalloc(fs->block_size);
		if (!xbuf)
			return ST_NOMEM;
		if (ext4_read_block(fs, in.i_file_acl_lo, xbuf) == ST_OK) {
			ext4_xattr_header *xh = (ext4_xattr_header *)xbuf;
			if (xh->h_magic == EXT4_XATTR_MAGIC) {
				if (!ext4_xattr_block_csum_ok(
					    fs, in.i_file_acl_lo, xbuf)) {
					kfree(xbuf);
					ext4_fs_error(
						fs,
						"xattr block checksum mismatch",
						ino);
					return ST_IO;
				}
				static ext4_xa_list LB;
				ext4_rwsem_write_lock(&s_xa_scratch);
				LB.n = 0;
				ext4_xa_parse(&LB,
					      xbuf + sizeof(ext4_xattr_header),
					      xbuf + fs->block_size, xbuf);
				ext4_xa_emit_names(&LB, buf, bufsz, &total,
						   &erange);
				ext4_xa_list_free(&LB);
				ext4_rwsem_write_unlock(&s_xa_scratch);
			}
		}
		kfree(xbuf);
	}
	if (erange)
		return ST_RANGE; /* -ERANGE */
	return (int)total;
}

/* setxattr/removexattr (value==NULL && size==0 with EXT4_XATTR_REPLACE-less means
 * set empty; the dedicated remove path passes remove=1).  Writes the rebuilt
 * ibody region back with a recomputed inode csum, journalled.  Returns ST_OK or
 * an ST_ error (ST_EXISTS, ST_NOT_FOUND, ST_NOSPC, ...). */
static int ext4_xattr_set_ino_locked(ext4_fs_t *fs, unsigned long ino,
				     int index, const char *name, unsigned nlen,
				     const void *value, unsigned long size,
				     int flags, int remove);

/* The parse lists below are static (too big for the stack) and shared by
 * every mounted ext4 filesystem, while the metadata lock that serialises
 * mutators is each filesystem's own: s_xa_scratch, which the readers take
 * for their lists, covers these as well. */
static int ext4_xattr_set_ino(ext4_fs_t *fs, unsigned long ino, int index,
			      const char *name, unsigned nlen,
			      const void *value, unsigned long size, int flags,
			      int remove)
{
	ext4_rwsem_write_lock(&s_xa_scratch);
	int r = ext4_xattr_set_ino_locked(fs, ino, index, name, nlen, value,
					  size, flags, remove);
	ext4_rwsem_write_unlock(&s_xa_scratch);
	return r;
}

static int ext4_xattr_set_ino_locked(ext4_fs_t *fs, unsigned long ino,
				     int index, const char *name, unsigned nlen,
				     const void *value, unsigned long size,
				     int flags, int remove)
{
	if (nlen > EXT4_XA_MAX_NAME)
		return ST_INVALID; /* nlen==0 = POSIX-ACL name */
	ext4_inode in;
	unsigned long blk;
	unsigned off, rstart, rsize;
	int st = ext4_xattr_ibody_region(fs, ino, &in, &blk, &off, &rstart,
					 &rsize);
	if (st != ST_OK)
		return st;

	uint8_t *bbuf =
		(uint8_t *)kalloc(fs->block_size); /* inode-table block    */
	uint8_t *xbuf =
		(uint8_t *)kalloc(fs->block_size); /* external xattr block */
	if (!bbuf || !xbuf) {
		if (bbuf)
			kfree(bbuf);
		if (xbuf)
			kfree(xbuf);
		return ST_NOMEM;
	}
	if (ext4_read_block(fs, blk, bbuf) != ST_OK) {
		kfree(bbuf);
		kfree(xbuf);
		return ST_IO;
	}

	ext4_inode *din = (ext4_inode *)(bbuf + off);
	unsigned long xblk =
		din->i_file_acl_lo; /* current external block (0=none) */
	int allocated_now = 0;
	int rc = ST_OK;

	static ext4_xa_list LI;
	LI.n = 0; /* in-inode (ibody) attributes */
	static ext4_xa_list LB;
	LB.n = 0; /* external-block attributes   */

	if (rsize) {
		uint8_t *region = bbuf + rstart;
		uint32_t mag;
		mm_memcpy(&mag, region, 4);
		if (mag == EXT4_XATTR_MAGIC)
			ext4_xa_parse(&LI,
				      region + sizeof(ext4_xattr_ibody_header),
				      bbuf + off + fs->inode_size,
				      region + sizeof(ext4_xattr_ibody_header));
	}
	if (xblk) {
		if (ext4_read_block(fs, xblk, xbuf) != ST_OK) {
			rc = ST_IO;
			goto out;
		}
		ext4_xattr_header *xh = (ext4_xattr_header *)xbuf;
		if (xh->h_magic == EXT4_XATTR_MAGIC) {
			if (!ext4_xattr_block_csum_ok(fs, xblk, xbuf)) {
				ext4_fs_error(fs,
					      "xattr block checksum mismatch",
					      ino);
				rc = ST_IO;
				goto out;
			}
			ext4_xa_parse(&LB, xbuf + sizeof(ext4_xattr_header),
				      xbuf + fs->block_size, xbuf);
		}
	}

	int ii = ext4_xa_find(&LI, index, name, nlen);
	int bi = ext4_xa_find(&LB, index, name, nlen);
	int existed = (ii >= 0) || (bi >= 0);

	if (remove) {
		if (!existed) {
			rc = ST_NODATA;
			goto out;
		}
	} else {
		if (existed && (flags & EXT4_XATTR_CREATE)) {
			rc = ST_EXISTS;
			goto out;
		}
		if (!existed && (flags & EXT4_XATTR_REPLACE)) {
			rc = ST_NODATA;
			goto out;
		}
	}

	/* Remove the target from whichever store currently holds it. */
	if (ii >= 0) {
		if (LI.a[ii].value)
			kfree(LI.a[ii].value);
		for (unsigned i = ii; i + 1 < LI.n; i++)
			LI.a[i] = LI.a[i + 1];
		LI.n--;
	}
	if (bi >= 0) {
		if (LB.a[bi].value)
			kfree(LB.a[bi].value);
		for (unsigned i = bi; i + 1 < LB.n; i++)
			LB.a[i] = LB.a[i + 1];
		LB.n--;
	}

	if (!remove) {
		uint8_t *nv = 0;
		if (size) {
			nv = (uint8_t *)kalloc(size);
			if (!nv) {
				rc = ST_NOMEM;
				goto out;
			}
			mm_memcpy(nv, value, size);
		}
		/* Prefer the ibody if the attribute fits there next to the others;
         * otherwise spill it to the external block. */
		int placed = 0;
		if (rsize && LI.n < EXT4_XA_MAX_ATTRS) {
			ext4_xa_attr *a = &LI.a[LI.n];
			a->index = (uint8_t)index;
			a->name_len = (uint8_t)nlen;
			mm_memcpy(a->name, name, nlen);
			a->value = nv;
			a->value_size = (uint32_t)size;
			LI.n++;
			if (ext4_xa_serial_size(
				    &LI, sizeof(ext4_xattr_ibody_header)) <=
			    rsize)
				placed = 1;
			else
				LI.n--; /* doesn't fit the inode slack -> use the block */
		}
		if (!placed) {
			if (LB.n >= EXT4_XA_MAX_ATTRS) {
				if (nv)
					kfree(nv);
				rc = ST_NOSPC;
				goto out;
			}
			ext4_xa_attr *a = &LB.a[LB.n++];
			a->index = (uint8_t)index;
			a->name_len = (uint8_t)nlen;
			mm_memcpy(a->name, name, nlen);
			a->value = nv;
			a->value_size = (uint32_t)size;
		}
	}

	/* Rewrite the ibody region. */
	if (rsize) {
		if (ext4_xa_serialize_ibody(bbuf + rstart, rsize, &LI) !=
		    ST_OK) {
			rc = ST_NOSPC;
			goto out;
		}
	} else if (LI.n) {
		rc = ST_NOSPC;
		goto out; /* (defensive: no ibody to hold them) */
	}

	/* Allocate / rewrite / free the external block as needed. */
	if (LB.n == 0) {
		if (xblk) { /* block emptied -> free it */
			ext4_free_block(fs, xblk);
			din->i_file_acl_lo = 0;
			unsigned long sub = fs->block_size / 512;
			din->i_blocks_lo = (din->i_blocks_lo >= sub) ?
						   din->i_blocks_lo - sub :
						   0;
			xblk = 0;
		}
	} else {
		if (ext4_xa_serial_size(&LB, sizeof(ext4_xattr_header)) >
		    fs->block_size) {
			rc = ST_NOSPC;
			goto out; /* too big for one block (nothing allocated) */
		}
		if (!xblk) {
			xblk = ext4_alloc_one_block(fs);
			if (!xblk) {
				rc = ST_NOSPC;
				goto out;
			}
			allocated_now = 1;
			din->i_file_acl_lo = (uint32_t)xblk;
			din->i_blocks_lo += fs->block_size / 512;
		}
		if (ext4_xa_serialize_block(fs, xbuf, xblk, &LB) != ST_OK ||
		    ext4_write_block(fs, xblk, xbuf) != ST_OK) {
			if (allocated_now)
				ext4_free_block(fs,
						xblk); /* undo the allocation */
			rc = ST_IO;
			goto out;
		}
	}

	ext4_inode_csum_set(fs, ino, bbuf + off); /* re-stamp the inode csum */
	if (ext4_write_block(fs, blk, bbuf) != ST_OK) {
		if (allocated_now)
			ext4_free_block(fs, xblk);
		rc = ST_IO;
		goto out;
	}
	ext4_inode_cache_drop(fs, ino);
out:
	ext4_xa_list_free(&LI);
	ext4_xa_list_free(&LB);
	kfree(bbuf);
	kfree(xbuf);
	return rc;
}

/* ---- directory entry insert / remove ---- */
static inline unsigned ext4_dirent_len(unsigned name_len)
{
	return (8 + name_len + 3) & ~3u;
}

/* ===================================================================
 * htree (hash-indexed directory) write support.
 *
 * On-disk an htree dir keeps its real entries in plain leaf blocks (read by the
 * linear scanner in ext4_dir_lookup_disk) plus an index: block 0 is a dx_root
 * (dot/dotdot + dx_root_info + a sorted {hash,logical-block} array), and for
 * deep trees, interior dx_node blocks.  We hash the name, descend the index to
 * the target leaf, insert there, and on a full leaf split it and propagate a new
 * index entry upward (growing the tree from depth 0 to 1 when the root fills).
 *
 * The dirhash (verified byte-exact against e2fsprogs) and the dx checksum are
 * the e2fsck gates; both are below.  A single index node holds up to ~510 leaf
 * pointers, so depth 1 covers far more entries than this system will ever put in
 * one directory; a full interior node (needing a node split / depth 2) returns
 * ST_NOSPC rather than build an unverified structure.
 * =================================================================== */

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

/* ---- the directory hash (mirrors e2fsprogs lib/ext2fs/dirhash.c) ---- */
#define EXT4_DX_DELTA 0x9E3779B9
static void ext4_dx_tea(uint32_t buf[4], const uint32_t in[4])
{
	uint32_t sum = 0, b0 = buf[0], b1 = buf[1];
	uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
	for (int n = 0; n < 16; n++) {
		sum += EXT4_DX_DELTA;
		b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
		b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
	}
	buf[0] += b0;
	buf[1] += b1;
}
#define DXF(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define DXG(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define DXH(x, y, z) ((x) ^ (y) ^ (z))
#define DXR(f, a, b, c, d, x, s) \
	(a += f(b, c, d) + (x), a = (a << (s)) | (a >> (32 - (s))))
static void ext4_dx_md4(uint32_t buf[4], const uint32_t in[8])
{
	uint32_t a = buf[0], b = buf[1], c = buf[2], d = buf[3];
	DXR(DXF, a, b, c, d, in[0], 3);
	DXR(DXF, d, a, b, c, in[1], 7);
	DXR(DXF, c, d, a, b, in[2], 11);
	DXR(DXF, b, c, d, a, in[3], 19);
	DXR(DXF, a, b, c, d, in[4], 3);
	DXR(DXF, d, a, b, c, in[5], 7);
	DXR(DXF, c, d, a, b, in[6], 11);
	DXR(DXF, b, c, d, a, in[7], 19);
	DXR(DXG, a, b, c, d, in[1] + 0x5A827999, 3);
	DXR(DXG, d, a, b, c, in[3] + 0x5A827999, 5);
	DXR(DXG, c, d, a, b, in[5] + 0x5A827999, 9);
	DXR(DXG, b, c, d, a, in[7] + 0x5A827999, 13);
	DXR(DXG, a, b, c, d, in[0] + 0x5A827999, 3);
	DXR(DXG, d, a, b, c, in[2] + 0x5A827999, 5);
	DXR(DXG, c, d, a, b, in[4] + 0x5A827999, 9);
	DXR(DXG, b, c, d, a, in[6] + 0x5A827999, 13);
	DXR(DXH, a, b, c, d, in[3] + 0x6ED9EBA1, 3);
	DXR(DXH, d, a, b, c, in[7] + 0x6ED9EBA1, 9);
	DXR(DXH, c, d, a, b, in[2] + 0x6ED9EBA1, 11);
	DXR(DXH, b, c, d, a, in[6] + 0x6ED9EBA1, 15);
	DXR(DXH, a, b, c, d, in[1] + 0x6ED9EBA1, 3);
	DXR(DXH, d, a, b, c, in[5] + 0x6ED9EBA1, 9);
	DXR(DXH, c, d, a, b, in[0] + 0x6ED9EBA1, 11);
	DXR(DXH, b, c, d, a, in[4] + 0x6ED9EBA1, 15);
	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
}
static void ext4_dx_str2hb(const char *msg, int len, uint32_t *buf, int num,
			   int sg)
{
	uint32_t pad = (uint32_t)len | ((uint32_t)len << 8);
	pad |= pad << 16;
	uint32_t val = pad;
	if (len > num * 4)
		len = num * 4;
	for (int i = 0; i < len; i++) {
		int c = sg ? (int)(signed char)msg[i] :
			     (int)(unsigned char)msg[i];
		val = (uint32_t)c + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}
static uint32_t ext4_dx_hack(const char *name, int len, int sg)
{
	uint32_t h0 = 0x12a3fe2d, h1 = 0x37abe8f9;
	while (len--) {
		int c = sg ? (int)(signed char)*name :
			     (int)(unsigned char)*name;
		name++;
		uint32_t h = h1 + (h0 ^ (uint32_t)(c * 7152373));
		if (h & 0x80000000u)
			h -= 0x7fffffff;
		h1 = h0;
		h0 = h;
	}
	return h0 << 1;
}
/* Major hash for placement; *minor optional.  Uses the fs hash seed if set. */
static uint32_t ext4_dx_hash(const ext4_fs_t *fs, int version, const char *name,
			     int len, uint32_t *minor_out)
{
	uint32_t buf[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
	int has_seed = 0;
	for (int i = 0; i < 4; i++)
		if (fs->sb_copy.s_hash_seed[i]) {
			has_seed = 1;
			break;
		}
	if (has_seed)
		for (int i = 0; i < 4; i++)
			buf[i] = fs->sb_copy.s_hash_seed[i];
	uint32_t hash = 0, minor = 0;
	uint32_t in[8];
	const char *p = name;
	int l = len;
	switch (version) {
	case EXT4_HASH_LEGACY_UNS:
		hash = ext4_dx_hack(name, len, 0);
		break;
	case EXT4_HASH_LEGACY:
		hash = ext4_dx_hack(name, len, 1);
		break;
	case EXT4_HASH_HALF_MD4_UNS:
	case EXT4_HASH_HALF_MD4:
		while (l > 0) {
			ext4_dx_str2hb(p, l, in, 8,
				       version == EXT4_HASH_HALF_MD4);
			ext4_dx_md4(buf, in);
			l -= 32;
			p += 32;
		}
		hash = buf[1];
		minor = buf[2];
		break;
	case EXT4_HASH_TEA_UNS:
	case EXT4_HASH_TEA:
		while (l > 0) {
			ext4_dx_str2hb(p, l, in, 4, version == EXT4_HASH_TEA);
			ext4_dx_tea(buf, in);
			l -= 16;
			p += 16;
		}
		hash = buf[0];
		minor = buf[1];
		break;
	default:
		while (l > 0) {
			ext4_dx_str2hb(p, l, in, 8, 1);
			ext4_dx_md4(buf, in);
			l -= 32;
			p += 32;
		}
		hash = buf[1];
		minor = buf[2];
		break; /* unknown -> half_md4 */
	}
	hash &= ~1u;
	if (hash == (uint32_t)(EXT4_HTREE_EOF << 1))
		hash = (uint32_t)((EXT4_HTREE_EOF - 1) << 1);
	if (minor_out)
		*minor_out = minor;
	return hash;
}

/* Checksum a dx index block (root if is_root, else interior node).  e2fsck only
 * recomputes the csum over the on-disk dt_reserved (we keep it 0), so this is
 * self-consistent and clean.  No-op without metadata_csum. */
static void ext4_dx_csum_set(const ext4_fs_t *fs, unsigned long dir_ino,
			     uint32_t gen, uint8_t *blk, int is_root)
{
	if (!fs->has_metadata_csum)
		return;
	unsigned count_off = is_root ? 32 : 8;
	ext4_dx_countlimit *cl = (ext4_dx_countlimit *)(blk + count_off);
	ext4_dx_tail *t =
		(ext4_dx_tail *)(blk + count_off + (unsigned)cl->limit * 8);
	t->dt_reserved = 0;
	uint32_t seed = ext4_inode_csum_seed(fs, dir_ino, gen);
	uint32_t csum =
		ext4_crc32c(seed, blk, count_off + (unsigned)cl->count * 8);
	csum = ext4_crc32c(csum, t, 4); /* dt_reserved (0) */
	uint32_t z = 0;
	csum = ext4_crc32c(csum, &z, 4);
	t->dt_checksum = csum;
}

/* dx entry-array capacity (entries incl. the count/limit slot), reserving the
 * 8-byte dx_tail when metadata_csum is on. */
static unsigned ext4_dx_root_limit(const ext4_fs_t *fs)
{
	return (fs->block_size - 32 - (fs->has_metadata_csum ? 8 : 0)) / 8;
}
static unsigned ext4_dx_node_limit(const ext4_fs_t *fs)
{
	return (fs->block_size - 8 - (fs->has_metadata_csum ? 8 : 0)) / 8;
}

/* Insert (name,child,ftype) into an in-memory leaf buffer if it fits (does not
 * write or checksum).  Returns 1 on success, 0 if no room. */
static int ext4_leaf_insert(ext4_fs_t *fs, uint8_t *blk, const char *name,
			    unsigned name_len, unsigned long child_ino,
			    unsigned ftype)
{
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	unsigned scan_limit = fs->block_size - tail;
	unsigned need = ext4_dirent_len(name_len);
	unsigned off = 0;
	while (off + 8 <= scan_limit) {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
		unsigned rec = de->rec_len;
		if (rec < 8 || off + rec > scan_limit)
			break;
		unsigned used =
			(de->inode == 0) ? 0 : ext4_dirent_len(de->name_len);
		if (rec - used >= need) {
			ext4_dir_entry_2 *ne;
			if (de->inode == 0) {
				ne = de;
				ne->rec_len = (uint16_t)rec;
			} else {
				de->rec_len = (uint16_t)used;
				ne = (ext4_dir_entry_2 *)(blk + off + used);
				ne->rec_len = (uint16_t)(rec - used);
			}
			ne->inode = (uint32_t)child_ino;
			ne->name_len = (uint8_t)name_len;
			ne->file_type = (uint8_t)ftype;
			mm_memcpy(ne->name, name, name_len);
			return 1;
		}
		off += rec;
	}
	return 0;
}

/* Append one block to directory `din` at its next logical index; persists the
 * inode (with the new extent + size) and invalidates the inode cache so the
 * block map sees it.  Returns the new logical block index, or ST_NOSPC. */
static long ext4_dir_grow(ext4_fs_t *fs, unsigned long dir_ino, ext4_inode *din)
{
	unsigned long nblocks = ext4_inode_size(din) / fs->block_size;
	if (ext4_alloc_blocks_for_file(fs, dir_ino, din, nblocks, 1) != 1)
		return ST_NOSPC;
	din->i_size_lo = (uint32_t)((nblocks + 1) * fs->block_size);
	if (ext4_write_inode_struct(fs, dir_ino, din) != ST_OK)
		return ST_IO;
	ext4_inode_cache_flush(fs);
	return (long)nblocks;
}

/* A sortable reference to a directory entry (in some block buffer) used while
 * redistributing entries during a leaf split. */
typedef struct {
	uint32_t hash;
	const ext4_dir_entry_2 *de;
} ext4_dx_ref;

static void ext4_dx_sort(ext4_dx_ref *r, int n) /* insertion sort by hash */
{
	for (int i = 1; i < n; i++) {
		ext4_dx_ref k = r[i];
		int j = i - 1;
		while (j >= 0 && r[j].hash > k.hash) {
			r[j + 1] = r[j];
			j--;
		}
		r[j + 1] = k;
	}
}

/* Lay refs[a..b) into a freshly initialised leaf block (no dot/dotdot), with the
 * last record padded to the block end, then checksum it. */
static void ext4_dx_build_leaf(ext4_fs_t *fs, unsigned long dir_ino,
			       uint32_t gen, uint8_t *blk,
			       const ext4_dx_ref *refs, int a, int b)
{
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	unsigned scan_limit = fs->block_size - tail;
	mm_memset(blk, 0, fs->block_size);
	unsigned off = 0, last_off = 0;
	for (int i = a; i < b; i++) {
		const ext4_dir_entry_2 *s = refs[i].de;
		unsigned rl = ext4_dirent_len(s->name_len);
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
		de->inode = s->inode;
		de->name_len = s->name_len;
		de->file_type = s->file_type;
		mm_memcpy(de->name, s->name, s->name_len);
		de->rec_len = (uint16_t)rl;
		last_off = off;
		off += rl;
	}
	if (b > a)
		((ext4_dir_entry_2 *)(blk + last_off))->rec_len =
			(uint16_t)(scan_limit - last_off);
	else {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)blk;
		de->inode = 0;
		de->rec_len = (uint16_t)scan_limit;
	}
	ext4_dir_csum_set(fs, dir_ino, gen, blk);
}

/* Insert (hash,block) into a dx index block's sorted entry array at sorted
 * position; assumes there is room (count < limit).  count_off = 32 root / 8 node. */
static void ext4_dx_insert_entry(uint8_t *blk, unsigned count_off,
				 uint32_t hash, uint32_t block)
{
	ext4_dx_countlimit *cl = (ext4_dx_countlimit *)(blk + count_off);
	ext4_dx_entry *e =
		(ext4_dx_entry *)(blk + count_off); /* e[0].hash aliases cl */
	unsigned count = cl->count;
	unsigned pos = 1;
	while (pos < count && (e[pos].hash & ~1u) <= hash)
		pos++; /* keep ascending order */
	for (unsigned i = count; i > pos; i--)
		e[i] = e[i - 1]; /* pos>=1 so e[0] kept */
	e[pos].hash = hash;
	e[pos].block = block;
	cl->count = (uint16_t)(count + 1);
}

/* Write a directory block by logical index (maps via the extent tree). */
static int ext4_dir_write_lblk(ext4_fs_t *fs, unsigned long dir_ino,
			       unsigned long lblk, const uint8_t *buf)
{
	unsigned long pbn = ext4_block_map(fs, dir_ino, lblk);
	if (pbn == 0)
		return ST_IO;
	return ext4_write_block(fs, pbn, buf);
}

/* Convert a single-block linear directory (block 0 full) into an htree: move its
 * real entries to a new leaf (logical block 1) and rebuild block 0 as a dx_root
 * with one entry covering the whole hash range.  Leaves the new name to the
 * caller's htree add. */
static int ext4_htree_make_indexed(ext4_fs_t *fs, unsigned long dir_ino,
				   ext4_inode *din)
{
	unsigned bs = fs->block_size;
	uint32_t gen = din->i_generation;
	int hv = fs->sb_copy.s_def_hash_version;
	if (hv < 0 || hv > 5)
		hv = EXT4_HASH_HALF_MD4;

	uint8_t *b0 = (uint8_t *)kalloc(bs);
	uint8_t *leaf = (uint8_t *)kalloc(bs);
	if (!b0 || !leaf) {
		if (b0)
			kfree(b0);
		if (leaf)
			kfree(leaf);
		return ST_NOMEM;
	}

	unsigned long pbn0 = ext4_block_map(fs, dir_ino, 0);
	if (pbn0 == 0 || ext4_read_block(fs, pbn0, b0) != ST_OK) {
		kfree(b0);
		kfree(leaf);
		return ST_IO;
	}

	/* Collect block 0's real entries (skip dot/dotdot) into the new leaf. */
	unsigned scan_limit =
		bs - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
	mm_memset(leaf, 0, bs);
	{
		ext4_dir_entry_2 *e0 = (ext4_dir_entry_2 *)
			leaf; /* one empty spanning record */
		e0->inode = 0;
		e0->rec_len = (uint16_t)scan_limit;
	}
	unsigned off = 0;
	unsigned long parent_ino = EXT4_ROOT_INO;
	unsigned long self_ino = dir_ino;
	while (off + 8 <= scan_limit) {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(b0 + off);
		unsigned rec = de->rec_len;
		if (rec < 8 || off + rec > scan_limit)
			break;
		if (de->inode != 0) {
			int isdot = (de->name_len == 1 && de->name[0] == '.');
			int isdd = (de->name_len == 2 && de->name[0] == '.' &&
				    de->name[1] == '.');
			if (isdd)
				parent_ino = de->inode;
			else if (isdot)
				self_ino = de->inode;
			else if (!ext4_leaf_insert(fs, leaf, de->name,
						   de->name_len, de->inode,
						   de->file_type)) {
				kfree(b0);
				kfree(leaf);
				return ST_NOSPC; /* shouldn't happen: same data, more room */
			}
		}
		off += rec;
	}

	long lblk = ext4_dir_grow(fs, dir_ino,
				  din); /* new leaf = logical block 1 */
	if (lblk < 0) {
		kfree(b0);
		kfree(leaf);
		return (int)lblk;
	}
	ext4_dir_csum_set(fs, dir_ino, gen, leaf);
	if (ext4_dir_write_lblk(fs, dir_ino, (unsigned long)lblk, leaf) !=
	    ST_OK) {
		kfree(b0);
		kfree(leaf);
		return ST_IO;
	}

	/* Rebuild block 0 as the dx_root. */
	mm_memset(b0, 0, bs);
	ext4_dir_entry_2 *dot = (ext4_dir_entry_2 *)b0;
	dot->inode = (uint32_t)self_ino;
	dot->rec_len = 12;
	dot->name_len = 1;
	dot->file_type = EXT4_FT_DIR;
	dot->name[0] = '.';
	ext4_dir_entry_2 *dd = (ext4_dir_entry_2 *)(b0 + 12);
	dd->inode = (uint32_t)parent_ino;
	dd->rec_len = (uint16_t)(bs - 12);
	dd->name_len = 2;
	dd->file_type = EXT4_FT_DIR;
	dd->name[0] = '.';
	dd->name[1] = '.';
	ext4_dx_root_info *info = (ext4_dx_root_info *)(b0 + 24);
	info->reserved_zero = 0;
	info->hash_version = (uint8_t)hv;
	info->info_length = 8;
	info->indirect_levels = 0;
	info->unused_flags = 0;
	ext4_dx_countlimit *cl = (ext4_dx_countlimit *)(b0 + 32);
	cl->limit = (uint16_t)ext4_dx_root_limit(fs);
	cl->count = 1;
	ext4_dx_entry *e = (ext4_dx_entry *)(b0 + 32);
	e[0].block = (uint32_t)lblk; /* whole hash range -> the one leaf */
	ext4_dx_csum_set(fs, dir_ino, gen, b0, 1);
	int st = ext4_dir_write_lblk(fs, dir_ino, 0, b0);
	kfree(b0);
	kfree(leaf);
	if (st != ST_OK)
		return st;

	din->i_flags |= EXT4_INODE_INDEX_FL;
	if (ext4_write_inode_struct(fs, dir_ino, din) != ST_OK)
		return ST_IO;
	ext4_inode_cache_flush(fs);
	return ST_OK;
}

/* Split the full leaf at logical `leaf_lblk` (whose contents are in `leafbuf`),
 * inserting the new entry; returns the lowest hash of the upper half in
 * *split_hash and the new leaf's logical block in *new_lblk. */
static int ext4_htree_split_leaf(ext4_fs_t *fs, unsigned long dir_ino,
				 ext4_inode *din, int hv, uint8_t *leafbuf,
				 unsigned long leaf_lblk, const char *name,
				 unsigned name_len, unsigned long child_ino,
				 unsigned ftype, uint32_t *split_hash,
				 unsigned long *new_lblk)
{
	unsigned bs = fs->block_size;
	uint32_t gen = din->i_generation;
	unsigned scan_limit =
		bs - (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
	/* Two fresh output buffers: the refs point INTO leafbuf, so neither output may
     * be leafbuf (ext4_dx_build_leaf memsets its destination first). */
	/* One ref per live entry plus the new one.  A dirent takes at least 8
	 * header bytes, so bs/8 + 1 always suffices -- a ref is 16 bytes, so
	 * sizing this as one block held only 256, and a leaf of short names
	 * (up to 340 in a 4 KiB block) could never be split. */
	unsigned cap = bs / 8 + 1;
	ext4_dx_ref *refs = (ext4_dx_ref *)kalloc(cap * sizeof(ext4_dx_ref));
	uint8_t *lowleaf = (uint8_t *)kalloc(bs);
	uint8_t *newleaf = (uint8_t *)kalloc(bs);
	if (!refs || !lowleaf || !newleaf) {
		if (refs)
			kfree(refs);
		if (lowleaf)
			kfree(lowleaf);
		if (newleaf)
			kfree(newleaf);
		return ST_NOMEM;
	}
	int n = 0;

	unsigned off = 0;
	while (off + 8 <= scan_limit) {
		ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(leafbuf + off);
		unsigned rec = de->rec_len;
		if (rec < 8 || off + rec > scan_limit)
			break;
		if (de->inode != 0 && (unsigned)n < cap) {
			refs[n].de = de;
			refs[n].hash =
				ext4_dx_hash(fs, hv, de->name, de->name_len, 0);
			n++;
		}
		off += rec;
	}
	/* Add the new entry via a synthetic dirent. */
	uint8_t newent[8 + 256];
	ext4_dir_entry_2 *nde = (ext4_dir_entry_2 *)newent;
	nde->inode = (uint32_t)child_ino;
	nde->rec_len = 0;
	nde->name_len = (uint8_t)name_len;
	nde->file_type = (uint8_t)ftype;
	mm_memcpy(nde->name, name, name_len);
	if ((unsigned)n >= cap) {
		kfree(refs);
		kfree(lowleaf);
		kfree(newleaf);
		return ST_NOSPC;
	}
	refs[n].de = nde;
	refs[n].hash = ext4_dx_hash(fs, hv, name, name_len, 0);
	n++;

	ext4_dx_sort(refs, n);

	/* Split by accumulated size so both halves fit. */
	unsigned total = 0;
	for (int i = 0; i < n; i++)
		total += ext4_dirent_len(refs[i].de->name_len);
	unsigned acc = 0;
	int m = 1;
	for (int i = 0; i < n; i++) {
		acc += ext4_dirent_len(refs[i].de->name_len);
		if (acc * 2 >= total) {
			m = i + 1;
			break;
		}
	}
	if (m < 1)
		m = 1;
	if (m > n - 1)
		m = n - 1;

	long nl = ext4_dir_grow(fs, dir_ino, din);
	if (nl < 0) {
		kfree(refs);
		kfree(lowleaf);
		kfree(newleaf);
		return (int)nl;
	}

	uint32_t sh = refs[m].hash;
	if (m > 0 && refs[m - 1].hash == refs[m].hash)
		sh |= 1; /* collision continuation */

	ext4_dx_build_leaf(fs, dir_ino, gen, lowleaf, refs, 0,
			   m); /* lower half -> old block */
	ext4_dx_build_leaf(fs, dir_ino, gen, newleaf, refs, m,
			   n); /* upper half -> new block */

	int st = ext4_dir_write_lblk(fs, dir_ino, leaf_lblk, lowleaf);
	if (st == ST_OK)
		st = ext4_dir_write_lblk(fs, dir_ino, (unsigned long)nl,
					 newleaf);
	kfree(refs);
	kfree(lowleaf);
	kfree(newleaf);
	if (st != ST_OK)
		return st;
	*split_hash = sh;
	*new_lblk = (unsigned long)nl;
	return ST_OK;
}

/* Add an entry to an htree directory (din already has INDEX_FL). */
static int ext4_htree_add(ext4_fs_t *fs, unsigned long dir_ino, ext4_inode *din,
			  const char *name, unsigned name_len,
			  unsigned long child_ino, unsigned ftype)
{
	unsigned bs = fs->block_size;
	uint32_t gen = din->i_generation;
	uint8_t *root = (uint8_t *)kalloc(bs);
	uint8_t *node = (uint8_t *)kalloc(bs);
	uint8_t *leaf = (uint8_t *)kalloc(bs);
	if (!root || !node || !leaf) {
		if (root)
			kfree(root);
		if (node)
			kfree(node);
		if (leaf)
			kfree(leaf);
		return ST_NOMEM;
	}

	int rc = ST_IO;
	unsigned long pbn = ext4_block_map(fs, dir_ino, 0);
	if (pbn == 0 || ext4_read_block(fs, pbn, root) != ST_OK)
		goto out;

	ext4_dx_root_info *info = (ext4_dx_root_info *)(root + 24);
	int hv = info->hash_version;
	int levels = info->indirect_levels;
	if (hv < 0 || hv > 5)
		hv = EXT4_HASH_HALF_MD4;
	uint32_t hash = ext4_dx_hash(fs, hv, name, name_len, 0);

	/* Descend root -> [node] -> leaf, remembering whether the parent of the leaf
     * is the root (depth 0) or the interior node (depth 1). */
	ext4_dx_entry *re = (ext4_dx_entry *)(root + 32);
	ext4_dx_countlimit *rcl = (ext4_dx_countlimit *)(root + 32);
	unsigned rcount = rcl->count, rat = 0;
	for (unsigned i = 1; i < rcount; i++) {
		if ((re[i].hash & ~1u) <= hash)
			rat = i;
		else
			break;
	}

	unsigned long leaf_lblk;
	uint8_t *parent;
	unsigned parent_off;
	unsigned long node_lblk = 0;
	int parent_is_root;
	if (levels == 0) {
		leaf_lblk = re[rat].block;
		parent = root;
		parent_off = 32;
		parent_is_root = 1;
	} else if (levels == 1) {
		node_lblk = re[rat].block;
		unsigned long npbn = ext4_block_map(fs, dir_ino, node_lblk);
		if (npbn == 0 || ext4_read_block(fs, npbn, node) != ST_OK)
			goto out;
		ext4_dx_entry *ne = (ext4_dx_entry *)(node + 8);
		ext4_dx_countlimit *ncl = (ext4_dx_countlimit *)(node + 8);
		unsigned ncount = ncl->count, nat = 0;
		for (unsigned i = 1; i < ncount; i++) {
			if ((ne[i].hash & ~1u) <= hash)
				nat = i;
			else
				break;
		}
		leaf_lblk = ne[nat].block;
		parent = node;
		parent_off = 8;
		parent_is_root = 0;
	} else {
		rc = ST_NOSPC;
		goto out;
	} /* depth>1: node split / depth 2 unsupported */

	unsigned long lpbn = ext4_block_map(fs, dir_ino, leaf_lblk);
	if (lpbn == 0 || ext4_read_block(fs, lpbn, leaf) != ST_OK)
		goto out;

	if (ext4_leaf_insert(fs, leaf, name, name_len, child_ino, ftype)) {
		ext4_dir_csum_set(fs, dir_ino, gen, leaf);
		rc = ext4_dir_write_lblk(fs, dir_ino, leaf_lblk, leaf);
		goto out;
	}

	/* Leaf full: split it, then add (split_hash,new_leaf) to the parent index. */
	uint32_t split_hash;
	unsigned long new_lblk;
	rc = ext4_htree_split_leaf(fs, dir_ino, din, hv, leaf, leaf_lblk, name,
				   name_len, child_ino, ftype, &split_hash,
				   &new_lblk);
	if (rc != ST_OK)
		goto out;
	/* din/extents changed in the split; re-read root & node to map blocks safely. */
	ext4_inode_cache_flush(fs);

	ext4_dx_countlimit *pcl = (ext4_dx_countlimit *)(parent + parent_off);
	if (pcl->count < pcl->limit) { /* room in the parent */
		ext4_dx_insert_entry(parent, parent_off, split_hash,
				     (uint32_t)new_lblk);
		ext4_dx_csum_set(fs, dir_ino, gen, parent, parent_is_root);
		rc = ext4_dir_write_lblk(
			fs, dir_ino, parent_is_root ? 0 : node_lblk, parent);
		goto out;
	}

	if (!parent_is_root) {
		rc = ST_NOSPC;
		goto out;
	} /* interior node full: node split unsupported */

	/* Root full at depth 0: grow to depth 1.  Move all root entries into a new
     * node block, point the root at it, then insert into the (roomy) node. */
	long nl = ext4_dir_grow(fs, dir_ino, din);
	if (nl < 0) {
		rc = (int)nl;
		goto out;
	}
	mm_memset(node, 0, bs);
	ext4_dir_entry_2 *fake = (ext4_dir_entry_2 *)node;
	fake->inode = 0;
	fake->rec_len = (uint16_t)bs;
	fake->name_len = 0;
	fake->file_type = 0;
	ext4_dx_entry *nentry = (ext4_dx_entry *)(node + 8);
	unsigned cnt = rcl->count;
	for (unsigned i = 0; i < cnt; i++)
		nentry[i] = re[i]; /* copies blocks (and hashes for i>=1) */
	ext4_dx_countlimit *ncl = (ext4_dx_countlimit *)(node + 8);
	ncl->limit = (uint16_t)ext4_dx_node_limit(fs);
	ncl->count = (uint16_t)cnt; /* aliases nentry[0].hash */
	ext4_dx_insert_entry(node, 8, split_hash, (uint32_t)new_lblk);
	ext4_dx_csum_set(fs, dir_ino, gen, node, 0);
	rc = ext4_dir_write_lblk(fs, dir_ino, (unsigned long)nl, node);
	if (rc != ST_OK)
		goto out;

	rcl->limit = (uint16_t)ext4_dx_root_limit(fs);
	rcl->count = 1;
	re[0].block = (uint32_t)nl; /* sole root entry -> the node */
	info->indirect_levels = 1;
	ext4_dx_csum_set(fs, dir_ino, gen, root, 1);
	rc = ext4_dir_write_lblk(fs, dir_ino, 0, root);

out:
	kfree(root);
	kfree(node);
	kfree(leaf);
	return rc;
}

static int ext4_dir_add_disk(ext4_fs_t *fs, unsigned long dir_ino,
			     const char *name, unsigned name_len,
			     unsigned long child_ino, unsigned ftype)
{
	ext4_inode din;
	if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
		return ST_IO;
	if (din.i_flags & EXT4_INODE_INDEX_FL) /* hash-indexed dir */
		return ext4_htree_add(fs, dir_ino, &din, name, name_len,
				      child_ino, ftype);
	unsigned long dsize = ext4_inode_size(&din);
	unsigned long nblocks = dsize / fs->block_size;
	unsigned need = ext4_dirent_len(name_len);
	/* With metadata_csum the last 12 bytes of every leaf hold the checksum tail
     * (a fake entry) and must never be allocated into. */
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	unsigned scan_limit = fs->block_size - tail;

	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return ST_NOMEM;

	for (unsigned long b = 0; b < nblocks; b++) {
		unsigned long pbn = ext4_block_map(fs, dir_ino, b);
		if (pbn == 0)
			continue;
		if (ext4_read_block(fs, pbn, blk) != ST_OK)
			continue;
		if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
			kfree(blk);
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			return ST_IO;
		}
		unsigned off = 0;
		while (off + 8 <= scan_limit) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > scan_limit)
				break;
			unsigned used = (de->inode == 0) ?
						0 :
						ext4_dirent_len(de->name_len);
			if (rec - used >= need) {
				ext4_dir_entry_2 *ne;
				if (de->inode == 0) {
					ne = de; /* reuse empty slot          */
					ne->rec_len = (uint16_t)rec;
				} else {
					de->rec_len = (uint16_t)
						used; /* shrink, split off the tail */
					ne = (ext4_dir_entry_2 *)(blk + off +
								  used);
					ne->rec_len = (uint16_t)(rec - used);
				}
				ne->inode = (uint32_t)child_ino;
				ne->name_len = (uint8_t)name_len;
				ne->file_type = (uint8_t)ftype;
				mm_memcpy(ne->name, name, name_len);
				ext4_dir_csum_set(fs, dir_ino, din.i_generation,
						  blk);
				int st = ext4_write_block(fs, pbn, blk);
				kfree(blk);
				return st;
			}
			off += rec;
		}
	}

	/* No room and this is still a single-block dir: if the fs supports dir_index,
     * convert to an htree (like the reference does at the 1->2 block transition)
     * and add via the index path. */
	if (nblocks == 1 && !(din.i_flags & EXT4_INODE_INDEX_FL) &&
	    (fs->sb_copy.s_feature_compat & EXT4_FEATURE_COMPAT_DIR_INDEX)) {
		kfree(blk);
		int cv = ext4_htree_make_indexed(fs, dir_ino, &din);
		if (cv != ST_OK)
			return cv;
		return ext4_htree_add(fs, dir_ino, &din, name, name_len,
				      child_ino, ftype);
	}

	/* No room in existing blocks: append a fresh directory block. */
	unsigned alloc =
		ext4_alloc_blocks_for_file(fs, dir_ino, &din, nblocks, 1);
	if (alloc != 1) {
		kfree(blk);
		return ST_NOMEM;
	}
	unsigned long pbn = ext4_block_map(fs, dir_ino, nblocks);
	/* ext4_block_map reads via the parsed cache; ensure it reflects new extent */
	ext4_inode_cache_flush(fs);
	if (pbn == 0) {
		/* extent just appended to din (in-memory); compute directly */
		ext4_extent_header *eh = (ext4_extent_header *)din.i_block;
		ext4_extent *ex = (ext4_extent *)(eh + 1);
		ext4_extent *last = &ex[eh->eh_entries - 1];
		unsigned len = last->ee_len;
		if (len > 32768)
			len -= 32768;
		pbn = ((unsigned long)last->ee_start_lo |
		       ((unsigned long)last->ee_start_hi << 32)) +
		      (nblocks - last->ee_block);
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
	if (st != ST_OK)
		return st;
	/* grow directory size + persist the inode (with the new extent). */
	din.i_size_lo = (uint32_t)(dsize + fs->block_size);
	ext4_write_inode_struct(fs, dir_ino, &din);
	return ST_OK;
}

/*
 * Add `name` to `dir_ino` and tell the dentry cache about it.
 *
 * The cache is updated only once the entry is on the device, so a failed add
 * leaves nothing claiming the name exists.  Every way this driver creates a
 * name -- create, mkdir, mknod, symlink, link, the destination half of a
 * rename -- arrives here, which is why the maintenance is here and not spread
 * across all of them.
 *
 * A name that had a negative entry gets it replaced rather than merely
 * dropped: the insert removes whatever was there first.  That is the case
 * where a program stats a file, does not find it, creates it, and stats it
 * again -- and must find it the second time.
 */
static int ext4_dir_add(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long child_ino,
			unsigned ftype)
{
	int st = ext4_dir_add_disk(fs, dir_ino, name, name_len, child_ino,
				   ftype);

	if (st == ST_OK)
		ext4_dc_add(fs, dir_ino, name, name_len, child_ino, ftype);
	return st;
}

/* Remove `name` from directory `dir_ino`.  Returns the removed child inode in
 * *out_child (and its file_type in *out_ft).  Coalesces the freed record into
 * the previous entry. */
static int ext4_dir_del_disk(ext4_fs_t *fs, unsigned long dir_ino,
			     const char *name, unsigned name_len,
			     unsigned long *out_child, unsigned *out_ft)
{
	ext4_inode din;
	if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
		return ST_IO;
	unsigned long nblocks = ext4_inode_size(&din) / fs->block_size;
	unsigned scan_limit = fs->block_size -
			      (fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0);
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return ST_NOMEM;

	for (unsigned long b = 0; b < nblocks; b++) {
		unsigned long pbn = ext4_block_map(fs, dir_ino, b);
		if (pbn == 0)
			continue;
		if (ext4_read_block(fs, pbn, blk) != ST_OK)
			continue;
		if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
			kfree(blk);
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			return ST_IO;
		}
		unsigned off = 0, prev = (unsigned)-1;
		while (off + 8 <= scan_limit) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > scan_limit)
				break;
			if (de->inode != 0 && de->name_len == name_len &&
			    ext4_memcmp(de->name, name, name_len) == 0) {
				if (out_child)
					*out_child = de->inode;
				if (out_ft)
					*out_ft = de->file_type;
				if (prev != (unsigned)-1) {
					ext4_dir_entry_2 *pd =
						(ext4_dir_entry_2 *)(blk +
								     prev);
					pd->rec_len =
						(uint16_t)(pd->rec_len + rec);
				} else {
					de->inode =
						0; /* first entry in block: just blank it */
				}
				ext4_dir_csum_set(fs, dir_ino, din.i_generation,
						  blk);
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

/*
 * Remove `name` from `dir_ino` and tell the dentry cache about it.
 *
 * The counterpart of ext4_dir_add, and the same argument: unlink, rmdir and
 * both halves of a rename all remove names through here, so this is the one
 * place that has to remember to say so.
 */
static int ext4_dir_del(ext4_fs_t *fs, unsigned long dir_ino, const char *name,
			unsigned name_len, unsigned long *out_child,
			unsigned *out_ft)
{
	unsigned long child = 0;
	unsigned ft = 0;
	int st;

	st = ext4_dir_del_disk(fs, dir_ino, name, name_len, &child, &ft);
	if (out_child)
		*out_child = child;
	if (out_ft)
		*out_ft = ft;
	if (st != ST_OK)
		return st;

	ext4_dc_del(fs, dir_ino, name, name_len);

	/* A name that led to a directory takes that directory's own cached
	 * entries with it.
	 *
	 * Two cases need it.  A removed directory leaves "." and ".." behind,
	 * keyed by its inode number, waiting for that number to be handed to
	 * something else.  And a directory MOVED by a rename keeps a ".."
	 * that now points at the parent it no longer has -- rename removes the
	 * old name through here, so dropping the entries here covers it.
	 *
	 * EXT4_FT_UNKNOWN is treated the same way: a filesystem without the
	 * filetype feature records every entry as unknown, and guessing wrong
	 * in that direction only costs a rescan. */
	if (ft == EXT4_FT_DIR || ft == EXT4_FT_UNKNOWN)
		ext4_dc_drop_dir(fs, child);
	return st;
}

/* Split a path into parent directory inode + final component name. */
static int ext4_resolve_parent(ext4_fs_t *fs, const char *path,
			       unsigned long *parent_ino, char *name_out,
			       unsigned cap)
{
	int last = -1;
	for (int i = 0; path[i]; i++)
		if (path[i] == '/')
			last = i;
	const char *base = (last >= 0) ? path + last + 1 : path;
	unsigned bl = 0;
	while (base[bl])
		bl++;
	if (bl == 0 || bl >= cap)
		return ST_INVALID;
	for (unsigned i = 0; i <= bl; i++)
		name_out[i] = base[i];

	if (last <= 0) { /* parent is root ('/foo') or cwd ('foo') */
		*parent_ino = (last == 0) ? EXT4_ROOT_INO : fs->st->cwd_ino;
		return ST_OK;
	}
	char pdir[256];
	if (last >= (int)sizeof(pdir))
		return ST_INVALID;
	for (int i = 0; i < last; i++)
		pdir[i] = path[i];
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
static void ext4_init_owner(unsigned puid, unsigned pgid, unsigned pmode,
			    int is_dir, unsigned *uid, unsigned *gid,
			    unsigned *mode)
{
	task_t *cur = sched_current();
	if (!cur) {
		*uid = puid;
		*gid = pgid;
		return;
	}
	*uid = (unsigned)cur->cred.fsuid;
	if (pmode & S_ISGID) {
		*gid = pgid;
		if (is_dir) {
			*mode |= S_ISGID; /* dirs always inherit it   */
		} else if (cur->cred.fsuid != 0 &&
			   (*mode & (S_ISGID | S_IXGRP)) ==
				   (S_ISGID | S_IXGRP) &&
			   !cred_in_group(&cur->cred, pgid)) {
			*mode &= ~(
				unsigned)S_ISGID; /* not a member: strip sgid */
		}
	} else {
		*gid = (unsigned)cur->cred.fsgid;
	}
}

/* Initialise + write a brand-new regular-file inode (owner/group/mode decided by
 * ext4_init_owner — the creating process, per the reference). */
/* Write a freshly allocated inode, zeroing the ENTIRE on-disk slot first so a
 * reused inode does not inherit the previous occupant's in-inode (ibody) xattrs.
 * ext4_write_inode_struct deliberately preserves the xattr tail (for updates),
 * which would be wrong here. */
static int ext4_write_inode_new(ext4_fs_t *fs, unsigned long ino,
				const ext4_inode *in)
{
	if (ino == 0 || ino > fs->inodes_count)
		return ST_INVALID;
	unsigned int group = (ino - 1) / fs->inodes_per_group;
	unsigned int index = (ino - 1) % fs->inodes_per_group;
	unsigned long itbl = ext4_inode_table_block(fs, &fs->gdt[group]);
	unsigned long byte = (unsigned long)index * fs->inode_size;
	unsigned long blk = itbl + byte / fs->block_size;
	unsigned off = byte % fs->block_size;
	uint8_t *buf = (uint8_t *)kalloc(fs->block_size);
	if (!buf)
		return ST_NOMEM;
	int st = ext4_read_block(fs, blk, buf);
	if (st != ST_OK) {
		kfree(buf);
		return st;
	}
	mm_memset(buf + off, 0,
		  fs->inode_size); /* clear slot incl. the xattr tail */
	unsigned copy = fs->inode_size < sizeof(ext4_inode) ?
				fs->inode_size :
				sizeof(ext4_inode);
	mm_memcpy(buf + off, in, copy);
	ext4_inode_csum_set(fs, ino, buf + off);
	st = ext4_write_block(fs, blk, buf);
	kfree(buf);
	return st;
}

/* Undo an inode allocation whose creation failed part-way.  The slot may
 * never have been written (a never-zeroed inode table leaves stale bytes in
 * it, a zeroed one a slot whose checksum is wrong), so it is first stored as
 * a deleted inode -- zeroed, dtime set, checksummed -- and only then released
 * the normal way, which reads it back.  Blocks the caller already attached
 * must have been freed before. */
static void ext4_abandon_new_inode(ext4_fs_t *fs, unsigned long ino, int is_dir)
{
	ext4_inode dead;
	mm_memset(&dead, 0, sizeof(dead));
	dead.i_dtime = (uint32_t)timer_get_epoch();
	if (fs->inode_size > 128)
		dead.i_extra_isize = 32;
	ext4_write_inode_new(fs, ino, &dead);
	ext4_inode_cache_drop(fs, ino);
	ext4_free_inode(fs, ino, is_dir);
}

static int ext4_create_inode(ext4_fs_t *fs, unsigned long ino, unsigned mode,
			     unsigned uid, unsigned gid)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)mode;
	ext4_inode_set_uid(&in, (uint32_t)uid);
	ext4_inode_set_gid(&in, (uint32_t)gid);
	in.i_links_count = 1;
	in.i_flags = EXT4_INODE_EXTENTS_FL;
	uint64_t now = timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME,
		   (int64_t)now, 0);
	ext4_extent_header *eh = (ext4_extent_header *)in.i_block;
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = 4;
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;
	return ext4_write_inode_new(fs, ino,
				    &in); /* zeroes stale ibody xattrs */
}

/*
 * Write zeros over [from, to) of a file, through the page cache.
 *
 * Allocation hands out blocks without clearing them, so a block that has just
 * become part of a file still holds what its previous owner put there.  Any
 * part of such a block that the write itself does not cover has to be cleared,
 * or reading it returns somebody else's data -- not merely wrong bytes, but
 * the contents of a file that was deleted.
 *
 * Doing it through the cache rather than the device means a read sees the
 * zeros at once instead of after a writeback, and a whole page of them costs
 * no device read on the way in.
 *
 * `file_size` is the size the file had BEFORE this write, which is what
 * decides whether a page is past the end and can be created empty.
 *
 * ONLY blocks the file actually has are written.  A range with no block
 * behind it is skipped, and that is not a shortcut -- it is the whole point:
 * an unmapped block already reads as zeros, so there is nothing stale in it to
 * cover.  Dirtying a page over one would also be actively wrong, because
 * writeback would then have nowhere to put it, would leave it dirty and
 * unreclaimable for good, and would say so once per page.
 *
 * Returns 0, or 1 if a page could not be obtained.
 */
static int ext4_zero_range(ext4_fs_t *fs, unsigned long chain_id,
			   unsigned long file_size, unsigned long from,
			   unsigned long to)
{
	unsigned long ino = EXT4_BID_INO(chain_id);
	unsigned long bs = fs->block_size;
	unsigned long done = 0;

	might_sleep();
	if (bs == 0)
		return 1;
	while (from < to) {
		/* One block at a time, because whether it needs clearing is a
		 * per-block question: a page can span several of them, and a
		 * file can have one mapped next to one that is not. */
		unsigned long lb = from / bs;
		unsigned long blk_end = (lb + 1) * bs;
		unsigned long seg_end = (to < blk_end) ? to : blk_end;

		if (ext4_block_map(fs, ino, lb) == 0) {
			from = seg_end; /* a hole: already reads as zeros */
			continue;
		}

		while (from < seg_end) {
			unsigned long page_idx = from / PAGE_SIZE;
			unsigned page_off = (unsigned)(from % PAGE_SIZE);
			unsigned avail = PAGE_SIZE - page_off;
			unsigned chunk = (seg_end - from < avail) ?
						 (unsigned)(seg_end - from) :
						 avail;
			int full_page = (page_off == 0 && chunk == PAGE_SIZE);
			pc_page_t *pg = pagecache_get_for_write(
				chain_id, page_idx, file_size, &fs->sb,
				chain_id, full_page);

			if (!pg || !pg->data)
				return 1;
			BUG_ON(page_off + chunk > PAGE_SIZE);
			mm_memset(pg->data + page_off, 0, chunk);
			ext4_ordered_add(fs, ino);
			pagecache_write_end(pg);
			from += chunk;

			/* A hole can be far larger than the write that created
			 * it -- a seek to a gigabyte followed by one byte is a
			 * gigabyte of it -- and every page of it is dirty and
			 * therefore unreclaimable until stored.  Give the
			 * writeback a turn on the way through rather than
			 * filling memory first. */
			if (++done >= PC_DIRTY_WRITEBACK_BATCH) {
				done = 0;
				pagecache_balance_dirty(&fs->sb);
			}
		}
	}
	return 0;
}

/* The write itself, at *posp -- the description's position for write(), a
 * position of the call's own for pwrite() -- which it moves past what it
 * wrote.  Called with the inode lock exclusive and the I/O lock. */
static long ext4_write_impl_at(vfs_file_t *f, const void *buf, long bytes,
			       unsigned long *posp)
{
	if (!f || !buf)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->fs)
		return ST_INVALID;
	if (ef->is_dir)
		return -EISDIR;
	if (bytes < 0)
		return ST_INVALID;
	if (bytes == 0)
		return 0;
	ext4_fs_t *fs = ef->fs;
	if (fs->read_only)
		return ST_ROFS; /* error latch / read-only mount */

	unsigned long wpos = *posp;
	unsigned long end = wpos + (unsigned long)bytes;
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ef->ino, &in, 0, 0) != ST_OK)
		return ST_IO;

	unsigned long cur_size = ext4_inode_size(&in);
	/* Whether this call gave the file new blocks.  Extents have to be on the
	 * device before the data they describe, so that case still stores the
	 * inode; a write within blocks the file already owns does not. */
	int allocated_blocks = 0;
	/* Whether the first / last block this write touches had to be created
	 * for it.  A block that did not exist a moment ago holds whatever its
	 * previous owner left, so the parts of it this write does NOT cover
	 * have to be cleared -- see the zeroing below. */
	int head_created = 0, tail_created = 0;

	/* A write past the end of the file is given blocks for the range it
	 * covers and nothing else: the stretch between the old end and the
	 * write stays a hole, which reads as zeros without owning a block.
	 *
	 * This used to allocate every block from the old end up to the new
	 * one.  A small write at a large offset -- a cache or database file
	 * written sparsely -- then owned the whole gap, and since a block that
	 * is mapped must not show its previous owner's data, the hole clearing
	 * below wrote zeros over all of it: gigabytes, synchronously, through
	 * the page cache to the device, inside a single write() and with the
	 * filesystem's metadata lock held the whole time.  Every other process
	 * that needed a page from this filesystem waited behind it.
	 *
	 * The loop below creates exactly the blocks the write lands on, inside
	 * the file or past its end alike. */

	/* The blocks the write lands on that the file does not have -- past
	 * its end, or in a hole inside it (a file extended by ftruncate has a
	 * size and no blocks at all; an earlier seek-and-write leaves a hole
	 * behind it).  The range this write covers is checked block by block,
	 * and whatever is missing is created.  Only that range -- the rest of
	 * the file stays sparse, which is what it is for.
	 */
	{
		unsigned long bs = fs->block_size;
		unsigned long first_lb = wpos / bs;
		unsigned long last_lb =
			(end > wpos) ? (end - 1) / bs : first_lb;
		unsigned long lb = first_lb;

		/* ext4_block_map reads the inode through the parsed cache, so
		 * an allocation it has not been told about still looks like a
		 * hole -- and this loop would then try to fill the same gap
		 * again, mapping two extents over one logical block.  Tell it
		 * first, and again after every allocation below. */
		ext4_inode_cache_update(fs, ef->ino, &in);

		while (lb <= last_lb) {
			unsigned long run_start, run = 0;

			if (ext4_block_map(fs, ef->ino, lb) != 0) {
				lb++;
				continue;
			}
			run_start = lb;
			while (lb <= last_lb &&
			       ext4_block_map(fs, ef->ino, lb) == 0) {
				run++;
				lb++;
			}

			unsigned got = ext4_alloc_blocks_for_file(
				fs, ef->ino, &in, run_start, (unsigned)run);

			if (got == 0) {
				ext4_flush_meta(fs);
				return -ENOSPC;
			}
			allocated_blocks = 1;
			if (run_start == first_lb)
				head_created = 1;
			if (run_start + got > last_lb)
				tail_created = 1;
			ext4_inode_cache_update(fs, ef->ino, &in);

			if (got < run) {
				/* Ran out part way through the gap: the write
				 * can only reach as far as what was created. */
				unsigned long max_end = (run_start + got) * bs;

				if (max_end <= wpos) {
					ext4_flush_meta(fs);
					return -ENOSPC;
				}
				end = max_end;
				bytes = (long)(end - wpos);
				break;
			}
		}
	}

	/* Persist the inode ONCE: new extents (if any) + final size + mtime, so
     * the data loop's block_map sees the extents and we avoid a second write. */
	unsigned long newsize = (end > cur_size) ? end : cur_size;
	in.i_size_lo = (uint32_t)newsize;
	if ((in.i_mode & S_IFMT) == S_IFREG)
		in.i_size_high = (uint32_t)(newsize >> 32);
	{
		uint64_t now = timer_get_epoch();
		ext4_stamp(fs, &in, EXT4_T_MTIME | EXT4_T_CTIME, (int64_t)now, 0);
	}
	/* The in-core copy is the current one from here on, and the block
	 * mapping that writeback performs reads through it -- so it is updated
	 * rather than dropped.  Emptying the whole cache, which is what this
	 * did, meant re-reading from the device the very inode the next line of
	 * this function needs. */
	ext4_inode_cache_update(fs, ef->ino, &in);

	/* The inode reaches the device now only if it has to.
	 *
	 * It has to when this write ALLOCATED, because the block map is part
	 * of the inode: a deferred copy of that would leave the filesystem not
	 * knowing it owns the blocks the data is about to go into, and the
	 * writeback that follows would have nowhere to put the pages.
	 *
	 * A write within blocks the file already owns changes only the size
	 * and the timestamps, and those are recorded instead -- one small
	 * memory update in place of a read-modify-write of a 4KB inode-table
	 * block through the journal, per write() call.  A program writing a
	 * file in 700-byte pieces crosses a block boundary once every six of
	 * them, so five out of six stop paying for it.
	 *
	 * What makes that safe is ext4_di_apply: every route to an inode puts
	 * the pending change back before handing it over, so open(), stat()
	 * and the writeback path all see the size the file actually has, not
	 * the one the device last heard about.  If there is no room to record
	 * it, it is stored the old way -- slower, never wrong.
	 *
	 * The record is made even when the inode is also stored below, and
	 * that is not redundant.  ext4_sb_write_inode -- the generic sync path
	 * -- takes the size from the per-open-handle copy in the generic inode
	 * cache UNLESS ext4_di_pending(fs) says this path has something better,
	 * and that copy is exactly the one its own comment warns "two handles
	 * on the same file can move backwards between them".  Recording only
	 * in the non-allocating case left the guard false for every write that
	 * GREW a file: a sync landing between the allocation and the next
	 * update of that handle wrote the shorter length back over the one
	 * this write computed.  The file then claims fewer blocks than it
	 * owns, and since ext4_sb_next_block derives the chain length from the
	 * size, the pages past the reverted end have nothing to map to --
	 * writeback finds no block, warns ("dirty page ... has no block behind
	 * it"), leaves them dirty and therefore unreclaimable, and the tail of
	 * the file never reaches the device. */
	int size_recorded =
		ext4_di_record(fs, ef->ino, newsize, in.i_mtime, in.i_ctime);
	if (allocated_blocks || !size_recorded) {
		ext4_write_inode_struct(fs, ef->ino, &in);
		/* Publish the copy that actually reached the device, not the
		 * one on the way there: ext4_write_inode_struct may adjust the
		 * inode as it stores it, and a cached copy missing those
		 * adjustments would hand them back to the next writer to store
		 * again. */
		ext4_inode_cache_update(fs, ef->ino, &in);
	}

	/* Write the data.  When journalling is on, full blocks are accumulated into
     * the persistent write-back buffer (st->wbounce) and flushed to disk as ONE big
     * device transfer (ext4_wb_flush) — coalescing many small write()s into a few
     * large USB commands, which is most of the sequential-write throughput.  The
     * buffer OUTLIVES the write() (that is the point); it is flushed when it fills,
     * on a non-contiguous write, before the journal commits the referencing
     * metadata (data=ordered, see ext4_journal_flush), and before any read/free of
     * these blocks.  Partial edge blocks are read-modify-written directly (after
     * flushing pending data so the on-disk block is current).  With NO journal,
     * data is written directly as before — no deferral. */
	unsigned long write_start = wpos;
	unsigned long pos = wpos, remaining = (unsigned long)bytes,
		      written = 0;
	unsigned long chain_id = EXT4_BID_ENC(ef->ino, 0);
	const uint8_t *src = (const uint8_t *)buf;
	int io_err = 0;
	/* The data goes into the page cache, not to the device.
	 *
	 * write() owes the caller that a later read sees these bytes.  It does
	 * not owe them a disk that has already been touched.  Putting them in
	 * the cache and marking the page dirty satisfies that at once, and
	 * leaves the device work to be done later, in large pieces, by
	 * writeback or by an explicit fsync.
	 *
	 * What this replaces went to the device inside write() itself, and had
	 * to: a write covering part of a block became a read of that block, a
	 * merge, and a write back, synchronously, every time.  A program
	 * writing a file in small pieces paid a device round trip per piece,
	 * and repeated writes to the same block paid it repeatedly.  Here the
	 * second and later writes to a page are memory copies.
	 *
	 * It also settles an inconsistency: reads have always come from this
	 * cache while writes went past it, so a file could be read back as its
	 * old contents until something dropped the page. */

	/* A write that starts past the end of the file leaves a gap behind it,
	 * and the gap must read as zeros.
	 *
	 * Most of it does by itself: the blocks between the old end and the
	 * write are not allocated (see above), and a hole reads as zeros.  What
	 * does not is the part of the old last block beyond the old end of
	 * file -- that block is mapped, and its tail holds whatever was there
	 * before (a previous owner's data, or bytes a truncate cut off).  So
	 * the gap is cleared through the same cache the data goes through,
	 * and ext4_zero_range skips every block the file does not have: only
	 * that tail, at most one block, is written.
	 *
	 * An ordinary append starts at exactly the old end of file, so there
	 * is no gap and this does nothing at all. */
	if (write_start > cur_size) {
		unsigned long hend = (write_start < end) ? write_start : end;

		if (ext4_zero_range(fs, chain_id, cur_size, cur_size, hend))
			io_err = 1;
	}

	/* The same argument for a block created INSIDE the file.
	 *
	 * Filling a gap gives the file blocks that are not new to the device,
	 * and this write covers only its own range of them.  Every block
	 * between the first and the last is covered end to end; the two at the
	 * edges can stick out, and what sticks out is still the previous
	 * owner's data.  Before this it read as zeros, because an unmapped
	 * block does, and it has to go on reading as zeros now that it is
	 * mapped. */
	if (!io_err && head_created) {
		unsigned long bs = fs->block_size;
		unsigned long head_from = (write_start / bs) * bs;

		if (head_from < write_start &&
		    ext4_zero_range(fs, chain_id, cur_size, head_from,
				    write_start))
			io_err = 1;
	}
	if (!io_err && tail_created && end > write_start) {
		unsigned long bs = fs->block_size;
		unsigned long tail_to = ((end - 1) / bs + 1) * bs;

		if (tail_to > end &&
		    ext4_zero_range(fs, chain_id, cur_size, end, tail_to))
			io_err = 1;
	}

	/* Only from here on is user memory read, so only from here on does the
	 * access check need lifting.  The hole above is kernel work. */
	smap_disable();
	while (remaining && !io_err) {
		unsigned long page_idx = pos / PAGE_SIZE;
		unsigned page_off = (unsigned)(pos % PAGE_SIZE);
		unsigned avail = PAGE_SIZE - page_off;
		unsigned chunk = (remaining < avail) ? (unsigned)remaining :
						       avail;
		/* Replacing the page entirely means its previous contents need
		 * not be fetched first. */
		int full_page = (page_off == 0 && chunk == PAGE_SIZE);
		pc_page_t *pg;

		/* Every block behind this page must already exist.
		 *
		 * The old loop checked this before writing, because it wrote to
		 * the block directly and needed its address.  Caching the write
		 * removes the need for the address here -- but not the need for
		 * the check.  A page with no block behind it cannot be written
		 * back at all: writeback would find nothing to map it to, leave
		 * it dirty, and move on.  The data would be lost with no error
		 * ever reported, the page would never become reclaimable, and
		 * enough of them would exhaust memory.  Refuse the write
		 * instead, exactly as before. */
		{
			unsigned long bs = fs->block_size;
			unsigned long first = ((unsigned long)page_idx *
					       PAGE_SIZE) /
					      bs;
			unsigned long nblk = (PAGE_SIZE + bs - 1) / bs;

			if (nblk == 0)
				nblk = 1;
			for (unsigned long b = 0; b < nblk; b++) {
				unsigned long byte_off =
					((unsigned long)page_idx * PAGE_SIZE) +
					b * bs;

				/* Only blocks this write actually covers. */
				if (byte_off >= pos + chunk)
					break;
				if (byte_off + bs <= pos)
					continue;
				if (ext4_block_map(fs, ef->ino, first + b) ==
				    0) {
					io_err = 1;
					break;
				}
			}
			if (io_err)
				break;
		}

		pg = pagecache_get_for_write(chain_id, page_idx, cur_size,
					     &fs->sb, chain_id, full_page);
		if (!pg || !pg->data) {
			io_err = 1;
			break;
		}
		BUG_ON(page_off + chunk > PAGE_SIZE);
		/* Two threads writing the same page do not race here, and it
		 * is worth saying why rather than leaving it to be worked out
		 * again: ext4_write takes the per-inode lock EXCLUSIVELY, so
		 * only one write() to a given file is ever inside this loop.
		 * Two threads writing the same page of the same file are
		 * serialised by that lock; two writing different files never
		 * share a page, because a page belongs to one inode.
		 *
		 * What the lock does NOT cover is reclaim and writeback on
		 * another processor, which is what the page is held for --
		 * see pagecache_get_for_write. */
		mm_memcpy(pg->data + page_off, src + written, chunk);
		/* This transaction now owes an ordered store for this file. */
		ext4_ordered_add(fs, ef->ino);
		/* Publish and release together.  The page was held from the
		 * moment it was handed over: until the copy is finished it is
		 * an ordinary clean page that reclaim on another processor
		 * would be entitled to take, and the copy would then land in
		 * somebody else's memory. */
		pagecache_write_end(pg);

		pos += chunk;
		written += chunk;
		remaining -= chunk;
	}
	smap_enable();

	if (written == 0 && io_err)
		return ST_IO;

	wpos += written;
	*posp = wpos;
	if (wpos > ef->size)
		ef->size = wpos;
	if (ef->inode)
		icache_update_size((ic_inode_t *)ef->inode, ef->size);
	/* The cached pages are NOT dropped here.
	 *
	 * They hold what was just written, and until writeback runs they are
	 * the only copy of it -- discarding them discards the write.  That is
	 * exactly what the line this replaces did once the data stopped going
	 * straight to the device: an append or an overwrite reported success
	 * and then read back as the file's previous contents, while a write
	 * starting at offset zero of an empty file survived, because the
	 * old-page test cannot be true for it.
	 *
	 * Dropping them was right while a write went past the cache and left it
	 * holding a stale copy.  The cache is now where the data is, and there
	 * is nothing else to tell.
	 *
	 * The block-chain cache still needs telling: the allocation above may
	 * have given the file new blocks, so the map from file offset to disk
	 * block has changed, and writeback is about to use it. */
	(void)write_start;
	icache_chain_invalidate(&fs->sb, EXT4_BID_ENC(ef->ino, 0));

	/* Having produced dirty pages, take a turn at storing them if too many
	 * are waiting.  Charged to the writer deliberately: it is what makes a
	 * program writing faster than the device can absorb slow down, instead
	 * of the machine filling with data it is not allowed to discard. */
	pagecache_balance_dirty(&fs->sb);
	ext4_flush_meta(fs); /* batch the deferred GDT/superblock writeback */
	return (long)written;
}

static int ext4_truncate_impl(vfs_file_t *f, unsigned long size)
{
	if (!f)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (ef->is_dir)
		return -EISDIR;

	ext4_inode in;
	if (ext4_read_inode_loc(fs, ef->ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	unsigned long cur = ext4_inode_size(&in);

	if (size < cur) {
		unsigned long from =
			(size + fs->block_size - 1) / fs->block_size;

		/* The part of the new last block past the new end must read
		 * as zeros when the file grows again (by a write past it, a
		 * truncate or an allocation), so it is cleared now, while the
		 * block is still the file's. */
		if (size % fs->block_size &&
		    ext4_zero_range(fs, EXT4_BID_ENC(ef->ino, 0), cur, size,
				    (unsigned long)from * fs->block_size))
			return ST_IO;

		/* Drop the cached pages BEFORE the blocks under them go away.
		 *
		 * The invalidation used to come twenty lines further down, past
		 * two inode writes, which left a window in which the file's
		 * dirty pages referred to blocks the allocator had already
		 * taken back.  A writeback landing there has two ways to be
		 * wrong and took both: for a page whose block is gone it finds
		 * nothing to store, warns, and leaves the page dirty -- so it
		 * is never reclaimable and the warning repeats for the life of
		 * the boot ("dirty page ... has no block behind it") -- and for
		 * one whose block has since been handed to another file, it
		 * writes this file's bytes into that one.
		 *
		 * Only the pages AT OR PAST the new end are dropped.  The ones
		 * below it are the part of the file that SURVIVES the truncate,
		 * and for a writer that has not synced they are the only copy
		 * of it -- discarding them loses data the caller never asked to
		 * lose.  They are stored first, while the blocks under them
		 * still belong to this file, so the store cannot land in
		 * another file's blocks. */
		pagecache_invalidate_range(&fs->sb, EXT4_BID_ENC(ef->ino, 0), size);
		pagecache_flush_file(&fs->sb, EXT4_BID_ENC(ef->ino, 0));
		ext4_free_blocks_from(fs, ef->ino, &in, from);
	}
	in.i_size_lo = (uint32_t)size;
	if ((in.i_mode & S_IFMT) == S_IFREG)
		in.i_size_high = (uint32_t)(size >> 32);
	uint64_t now = timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_MTIME | EXT4_T_CTIME, (int64_t)now, 0);
	ext4_inode_cache_flush(fs);
	/* Any size a write left pending is superseded by this one, and dropped
	 * before the store rather than after it: if the store fails the file
	 * keeps the size it has on the device, which is what a failed truncate
	 * should leave behind -- not the length it had before the truncate was
	 * asked for. */
	ext4_di_clear(fs, ef->ino);
	int st = ext4_write_inode_struct(fs, ef->ino, &in);
	ef->size = size;
	if (ef->pos > size)
		ef->pos = size;
	if (ef->inode)
		icache_update_size((ic_inode_t *)ef->inode, size);
	/* NOTHING is invalidated here, and nothing may be.
	 *
	 * This used to drop the file's whole page cache on EVERY truncate,
	 * including the ones that free no blocks at all -- and
	 * pagecache_invalidate_file(&fs->sb, ) is a DISCARD: it takes each page off the
	 * dirty list and frees the frame without storing it.
	 *
	 * ftruncate(fd, size) with size equal to the length just written is a
	 * routine end-of-transfer step rather than an odd one -- scp(1) ends
	 * every incoming file with it -- and it threw the entire transfer away.
	 * The inode kept the new size and the extents kept the new blocks, but
	 * the bytes never left memory, so reading the file back returned
	 * whatever the previous owner of those blocks had left in them: a
	 * freshly copied file came out the right length and full of some other
	 * file's contents.
	 *
	 * The shrink path above drops exactly the pages whose blocks it frees,
	 * at the point where doing so is correct, so there is nothing left for
	 * this to invalidate.  A truncate that grows the file, or leaves the
	 * length alone, has no stale page to drop in the first place. */
	icache_chain_invalidate(&fs->sb, EXT4_BID_ENC(ef->ino, 0));
	ext4_flush_meta(fs);
	return st == ST_OK ? ST_OK : ST_IO;
}

static int ext4_unlink_impl(ext4_fs_t *fs, const char *path)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;

	unsigned long child = 0;
	unsigned ft = 0;
	/* Look up first so we can reject directories and recover the inode. */
	if (ext4_dir_lookup(fs, parent, name, nl, &child, &ft) != ST_OK)
		return ST_NOT_FOUND;
	ext4_inode cin;
	if (ext4_read_inode_loc(fs, child, &cin, 0, 0) != ST_OK)
		return ST_IO;
	if ((cin.i_mode & S_IFMT) == S_IFDIR)
		return ST_ISDIR;

	if (ext4_dir_del(fs, parent, name, nl, 0, 0) != ST_OK)
		return ST_NOT_FOUND;

	if (cin.i_links_count > 0)
		cin.i_links_count--;
	if (cin.i_links_count == 0 &&
	    icache_flag_if_referenced(&fs->sb, EXT4_BID_ENC(child, 0), IC_ORPHAN)) {
		/* Still open somewhere.  The name is gone, but the inode and
		 * its blocks stay until the last close (ext4_orphan_reap):
		 * freeing them now hands the number to the next mkdir or
		 * creat, and the open handle's later writes then land in
		 * THAT file -- seen as a fresh directory whose size fell
		 * back to zero because a temp file's writer outlived its
		 * unlink.  The reference keeps such inodes on its orphan
		 * list for the same reason. */
		ext4_stamp_now(fs, &cin, EXT4_T_CTIME);
		ext4_write_inode_struct(fs, child, &cin);
	} else if (cin.i_links_count == 0) {
		/* Cache before blocks -- see ext4_truncate_impl().  An unlinked
		 * file can still have dirty pages: a writer that closed without
		 * fsync, or a reader still holding it open. */
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
		ext4_free_blocks_from(fs, child, &cin, 0);
		cin.i_dtime = (uint32_t)timer_get_epoch();
		cin.i_size_lo = 0;
		cin.i_size_high = 0;
		ext4_write_inode_struct(fs, child, &cin);
		ext4_free_inode(fs, child, 0);
		icache_remove(&fs->sb, EXT4_BID_ENC(child, 0));
		pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
	} else {
		ext4_stamp_now(fs, &cin, EXT4_T_CTIME);
		ext4_write_inode_struct(fs, child, &cin);
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

/* Fill a fresh directory block with "." (self) and ".." (parent) entries. */
static void ext4_init_dir_block(ext4_fs_t *fs, uint8_t *blk,
				unsigned long self_ino,
				unsigned long parent_ino, uint32_t gen)
{
	unsigned bs = fs->block_size;
	unsigned tail = fs->has_metadata_csum ? EXT4_DIR_TAIL_SIZE : 0;
	mm_memset(blk, 0, bs);
	ext4_dir_entry_2 *dot = (ext4_dir_entry_2 *)blk;
	dot->inode = (uint32_t)self_ino;
	dot->rec_len = 12;
	dot->name_len = 1;
	dot->file_type = EXT4_FT_DIR;
	dot->name[0] = '.';
	ext4_dir_entry_2 *dd = (ext4_dir_entry_2 *)(blk + 12);
	dd->inode = (uint32_t)parent_ino;
	dd->rec_len = (uint16_t)(bs - 12 - tail);
	dd->name_len = 2;
	dd->file_type = EXT4_FT_DIR;
	dd->name[0] = '.';
	dd->name[1] = '.';
	ext4_dir_csum_set(fs, self_ino, gen,
			  blk); /* lay out + checksum the tail */
}

/* Return 1 if the directory contains only "." and "..". */
static int ext4_dir_is_empty(ext4_fs_t *fs, unsigned long dir_ino)
{
	ext4_inode din;
	if (ext4_read_inode_loc(fs, dir_ino, &din, 0, 0) != ST_OK)
		return 0;
	unsigned long nblocks = ext4_inode_size(&din) / fs->block_size;
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return 0;
	int empty = 1;
	for (unsigned long b = 0; b < nblocks && empty; b++) {
		unsigned long pbn = ext4_block_map(fs, dir_ino, b);
		if (pbn == 0)
			continue;
		if (ext4_read_block(fs, pbn, blk) != ST_OK)
			continue;
		if (!ext4_dir_csum_ok(fs, dir_ino, din.i_generation, blk)) {
			kfree(blk);
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			return 0; /* corrupt -> treat as "not empty": refuse the rmdir */
		}
		unsigned off = 0;
		while (off + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > fs->block_size)
				break;
			if (de->inode != 0 &&
			    !((de->name_len == 1 && de->name[0] == '.') ||
			      (de->name_len == 2 && de->name[0] == '.' &&
			       de->name[1] == '.'))) {
				empty = 0;
				break;
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
	ext4_inode di;
	uint32_t gen = ext4_get_inode_cached(fs, dir_ino, &di) ?
			       di.i_generation :
			       0;
	unsigned long pbn = ext4_block_map(fs, dir_ino, 0);
	if (pbn == 0)
		return;
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk)
		return;
	if (ext4_read_block(fs, pbn, blk) == ST_OK) {
		if (!ext4_dir_csum_ok(fs, dir_ino, gen, blk)) {
			ext4_fs_error(fs, "directory leaf checksum mismatch",
				      dir_ino);
			kfree(blk);
			return;
		}
		unsigned off = 0;
		while (off + 8 <= fs->block_size) {
			ext4_dir_entry_2 *de = (ext4_dir_entry_2 *)(blk + off);
			unsigned rec = de->rec_len;
			if (rec < 8 || off + rec > fs->block_size)
				break;
			if (de->name_len == 2 && de->name[0] == '.' &&
			    de->name[1] == '.') {
				de->inode = (uint32_t)new_parent;
				ext4_dir_csum_set(fs, dir_ino, gen,
						  blk); /* re-stamp the tail */
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
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return;
	if (delta < 0 && in.i_links_count < (unsigned)(-delta))
		in.i_links_count = 0;
	else
		in.i_links_count = (uint16_t)(in.i_links_count + delta);
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	ext4_write_inode_struct(fs, ino, &in);
}

static int ext4_mkdir_impl(ext4_fs_t *fs, const char *path, unsigned mode)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned long existing;
	if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK)
		return ST_EXISTS;

	/* Owner/group of the new directory per the reference (the creating process;
     * a set-group-ID parent passes its group down and the new dir inherits the
     * set-group-ID bit via ext4_init_owner). */
	unsigned puid = 0, pgid = 0, pmode = 0;
	ext4_inode pin0;
	if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
		puid = ext4_inode_uid(&pin0);
		pgid = ext4_inode_gid(&pin0);
		pmode = pin0.i_mode;
	}
	unsigned nuid = puid, ngid = pgid;
	unsigned nmode = S_IFDIR | (mode ? (mode & 0777) : 0755);
	ext4_init_owner(puid, pgid, pmode, 1, &nuid, &ngid, &nmode);
	unsigned long nino = ext4_alloc_inode(fs, parent, 1);
	if (nino == 0)
		return ST_NOMEM;

	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)nmode;
	ext4_inode_set_uid(&in, (uint32_t)nuid);
	ext4_inode_set_gid(&in, (uint32_t)ngid);
	in.i_links_count = 2; /* "." + the entry in parent       */
	in.i_flags = EXT4_INODE_EXTENTS_FL;
	uint32_t now = (uint32_t)timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME,
		   (int64_t)now, 0);
	ext4_extent_header *eh = (ext4_extent_header *)in.i_block;
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = 4;
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;

	if (ext4_alloc_blocks_for_file(fs, nino, &in, 0, 1) != 1) {
		ext4_abandon_new_inode(fs, nino, 1);
		ext4_flush_meta(fs);
		return ST_NOMEM;
	}
	in.i_size_lo = fs->block_size;
	ext4_extent *ex = (ext4_extent *)(eh + 1);
	unsigned long pbn = (unsigned long)ex[0].ee_start_lo |
			    ((unsigned long)ex[0].ee_start_hi << 32);
	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	if (!blk) {
		ext4_free_blocks_from(fs, nino, &in, 0);
		ext4_abandon_new_inode(fs, nino, 1);
		ext4_flush_meta(fs);
		return ST_NOMEM;
	}
	ext4_init_dir_block(fs, blk, nino, parent, in.i_generation);
	ext4_write_block(fs, pbn, blk); /* dir block is metadata           */
	kfree(blk);
	ext4_inode_cache_flush(fs);
	ext4_write_inode_new(fs, nino,
			     &in); /* fresh inode: clear stale ibody xattrs */

	if (ext4_dir_add(fs, parent, name, nl, nino, EXT4_FT_DIR) != ST_OK) {
		ext4_free_blocks_from(fs, nino, &in, 0);
		ext4_abandon_new_inode(fs, nino, 1);
		ext4_flush_meta(fs);
		return ST_IO;
	}
	ext4_adjust_links(fs, parent,
			  +1); /* new dir's ".." references parent */
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

/* Create a node that owns no data blocks: a socket or a FIFO.
 *
 * Simpler than ext4_mkdir_impl above — there is no directory block to
 * allocate, no "." / ".." to write, and the parent's link count is untouched.
 * The point of it is that a bound AF_UNIX socket has to EXIST in the
 * filesystem: clients stat() the path and refuse to connect unless it reports
 * S_IFSOCK, so an absent entry (or a plain file standing in for one) fails
 * every connect rather than merely looking untidy in ls. */
/* The directory-entry file type for an inode mode. */
static unsigned ext4_mode_to_ft(unsigned mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return EXT4_FT_DIR;
	case S_IFLNK:
		return EXT4_FT_SYMLINK;
	case S_IFCHR:
		return EXT4_FT_CHRDEV;
	case S_IFBLK:
		return EXT4_FT_BLKDEV;
	case S_IFIFO:
		return EXT4_FT_FIFO;
	case S_IFSOCK:
		return EXT4_FT_SOCK;
	default:
		return EXT4_FT_REG_FILE;
	}
}

/* Create a node that owns no blocks: a socket, a FIFO, or a character or
 * block device node.  Such an inode has neither the extents flag nor an
 * extent header -- fsck rejects a special file that claims to map blocks --
 * and a device node keeps its number in i_block: the old encoding (major
 * and minor below 256, in i_block[0]) when it fits, the new one (in
 * i_block[1], i_block[0] zero) otherwise. */
static int ext4_mknod_impl(ext4_fs_t *fs, const char *path, unsigned mode,
			   uint64_t dev)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned type = mode & S_IFMT;
	if (type != S_IFSOCK && type != S_IFIFO && type != S_IFCHR &&
	    type != S_IFBLK)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned long existing;
	if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK)
		return ST_EXISTS;

	unsigned puid = 0, pgid = 0, pmode = 0;
	ext4_inode pin0;
	if (ext4_read_inode_loc(fs, parent, &pin0, 0, 0) == ST_OK) {
		puid = ext4_inode_uid(&pin0);
		pgid = ext4_inode_gid(&pin0);
		pmode = pin0.i_mode;
	}
	unsigned nuid = puid, ngid = pgid;
	unsigned nmode = mode;
	ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);

	unsigned long nino = ext4_alloc_inode(fs, parent, 0);
	if (nino == 0)
		return ST_NOMEM;

	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)nmode;
	ext4_inode_set_uid(&in, (uint32_t)nuid);
	ext4_inode_set_gid(&in, (uint32_t)ngid);
	in.i_links_count = 1;
	in.i_flags = 0;
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;
	ext4_stamp_now(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME);
	if (type == S_IFCHR || type == S_IFBLK) {
		uint32_t ma = kdev_major(dev), mi = kdev_minor(dev);
		uint32_t w[2] = { 0, 0 };

		if (ma < 256 && mi < 256)
			w[0] = (ma << 8) | mi;
		else
			w[1] = (mi & 0xffu) | (ma << 8) | ((mi & ~0xffu) << 12);
		mm_memcpy(in.i_block, w, sizeof(w));
	}
	ext4_inode_cache_flush(fs);
	ext4_write_inode_new(fs, nino, &in);

	unsigned ft = ext4_mode_to_ft(mode);

	if (ext4_dir_add(fs, parent, name, nl, nino, ft) != ST_OK) {
		ext4_abandon_new_inode(fs, nino, 0);
		ext4_flush_meta(fs);
		return ST_IO;
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

static int ext4_rmdir_impl(ext4_fs_t *fs, const char *path)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, path, &parent, name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned nl = 0;
	while (name[nl])
		nl++;
	if (nl == 1 && name[0] == '.')
		return ST_INVALID;

	unsigned long child = 0;
	unsigned ft = 0;
	if (ext4_dir_lookup(fs, parent, name, nl, &child, &ft) != ST_OK)
		return ST_NOT_FOUND;
	ext4_inode cin;
	if (ext4_read_inode_loc(fs, child, &cin, 0, 0) != ST_OK)
		return ST_IO;
	if ((cin.i_mode & S_IFMT) != S_IFDIR)
		return ST_NOTDIR;
	if (!ext4_dir_is_empty(fs, child))
		return ST_NOTEMPTY;

	if (ext4_dir_del(fs, parent, name, nl, 0, 0) != ST_OK)
		return ST_NOT_FOUND;
	/* Cache before blocks -- see ext4_truncate_impl(). */
	pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
	ext4_free_blocks_from(fs, child, &cin, 0);
	cin.i_links_count = 0;
	cin.i_size_lo = 0;
	cin.i_dtime = (uint32_t)timer_get_epoch();
	ext4_write_inode_struct(fs, child, &cin);
	ext4_free_inode(fs, child, 1);
	icache_remove(&fs->sb, EXT4_BID_ENC(child, 0));
	pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(child, 0));
	ext4_adjust_links(fs, parent, -1); /* child's ".." is gone            */
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

static int ext4_rename_impl(ext4_fs_t *fs, const char *oldp, const char *newp)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!oldp || !newp || !fs)
		return ST_INVALID;
	unsigned long op = 0, np = 0;
	char oname[256], nname[256];
	if (ext4_resolve_parent(fs, oldp, &op, oname, sizeof(oname)) != ST_OK)
		return ST_NOT_FOUND;
	if (ext4_resolve_parent(fs, newp, &np, nname, sizeof(nname)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned onl = 0;
	while (oname[onl])
		onl++;
	unsigned nnl = 0;
	while (nname[nnl])
		nnl++;

	unsigned long src = 0;
	unsigned sft = 0;
	if (ext4_dir_lookup(fs, op, oname, onl, &src, &sft) != ST_OK)
		return ST_NOT_FOUND;
	ext4_inode sin;
	if (ext4_read_inode_loc(fs, src, &sin, 0, 0) != ST_OK)
		return ST_IO;
	int src_is_dir = ((sin.i_mode & S_IFMT) == S_IFDIR);

	/* A directory cannot move below itself: walking up from the new
	 * parent must not meet it, or the subtree would hang off nothing. */
	if (src_is_dir && op != np) {
		unsigned long cur = np;

		for (unsigned depth = 0; cur != EXT4_ROOT_INO; depth++) {
			unsigned long up = 0;

			if (cur == src)
				return ST_INVALID;
			if (depth > 4096 ||
			    ext4_dir_lookup(fs, cur, "..", 2, &up, 0) !=
				    ST_OK ||
			    up == cur)
				break;
			cur = up;
		}
	}

	/* If the destination exists, remove it (only an existing *file* — leave
     * directory replacement to a later increment to keep link accounting
     * simple and correct).  A file never replaces a directory, nor a
     * directory a file. */
	unsigned long dst = 0;
	unsigned dft = 0;
	if (ext4_dir_lookup(fs, np, nname, nnl, &dst, &dft) == ST_OK) {
		if (dst == src)
			return ST_OK; /* renaming to itself              */
		ext4_inode din;
		if (ext4_read_inode_loc(fs, dst, &din, 0, 0) != ST_OK)
			return ST_IO;
		int dst_is_dir = (din.i_mode & S_IFMT) == S_IFDIR;

		if (dst_is_dir && !src_is_dir)
			return ST_ISDIR;
		if (!dst_is_dir && src_is_dir)
			return ST_NOTDIR;
		if (dst_is_dir && !ext4_dir_is_empty(fs, dst))
			return ST_NOTEMPTY;
		if (dst_is_dir)
			return ST_EXISTS;
		ext4_dir_del(fs, np, nname, nnl, 0, 0);
		if (din.i_links_count > 0)
			din.i_links_count--;
		if (din.i_links_count == 0 &&
		    icache_flag_if_referenced(&fs->sb, EXT4_BID_ENC(dst, 0), IC_ORPHAN)) {
			/* Open somewhere: kept until its last close, as in
			 * ext4_unlink_impl(). */
			ext4_stamp_now(fs, &din, EXT4_T_CTIME);
			ext4_write_inode_struct(fs, dst, &din);
		} else if (din.i_links_count == 0) {
			/* Cache before blocks -- see ext4_truncate_impl().
			 * The file being replaced is an ordinary one and may
			 * well have dirty pages of its own. */
			pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(dst, 0));
			ext4_free_blocks_from(fs, dst, &din, 0);
			din.i_size_lo = 0;
			din.i_dtime = (uint32_t)timer_get_epoch();
			ext4_write_inode_struct(fs, dst, &din);
			ext4_free_inode(fs, dst, 0);
			icache_remove(&fs->sb, EXT4_BID_ENC(dst, 0));
			pagecache_invalidate_file(&fs->sb, EXT4_BID_ENC(dst, 0));
		} else {
			ext4_write_inode_struct(fs, dst, &din);
		}
		ext4_inode_cache_flush(fs);
	}

	if (ext4_dir_add(fs, np, nname, nnl, src, sft) != ST_OK)
		return ST_IO;
	ext4_dir_del(fs, op, oname, onl, 0, 0);

	if (src_is_dir && op != np) {
		ext4_dir_set_dotdot(fs, src, np);
		ext4_adjust_links(fs, op,
				  -1); /* src's old ".." left op          */
		ext4_adjust_links(fs, np,
				  +1); /* src's new ".." references np     */
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	return ST_OK;
}

/* Set one timestamp per struct vfs_utimes (seconds + nanoseconds, "now" or
 * "leave alone"). */
static void ext4_apply_utime(const ext4_fs_t *fs, ext4_inode *in,
			     unsigned which, int64_t sec, long nsec)
{
	if (nsec == VFS_UTIME_OMIT)
		return;
	if (nsec == VFS_UTIME_NOW) {
		ext4_stamp_now(fs, in, which);
		return;
	}
	if (nsec < 0 || nsec > 999999999L)
		nsec = 0;
	ext4_stamp(fs, in, which, sec, (uint32_t)nsec);
}

/* utimensat: the access and modification times of `ino' as `t' asks, and
 * the change time to now, as any change of an inode's attributes does.
 * Under the I/O lock. */
static int ext4_utimens_ino(ext4_fs_t *fs, unsigned long ino,
			    const struct vfs_utimes *t)
{
	/* This inode's own pending size/time update (and only its own: the
	 * other files' stay deferred) goes to the device with the times
	 * set here -- the read below brings it along, and the store clears
	 * it, so it cannot later put the old modification time back over
	 * this one.  A size may only reach the device after the data it
	 * covers, so the file's pages are stored first, before the inode is
	 * read: whatever storing them changes in the inode is then in what
	 * is written back. */
	if (ext4_di_pending(fs, ino)) {
		unsigned long id = EXT4_BID_ENC(ino, 0);

		pagecache_flush_fileset(&fs->sb, &id, 1);
	}
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	ext4_apply_utime(fs, &in, EXT4_T_ATIME, t->atime_sec, t->atime_nsec);
	ext4_apply_utime(fs, &in, EXT4_T_MTIME, t->mtime_sec, t->mtime_nsec);
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	int rc = ext4_write_inode_struct(fs, ino, &in);
	return rc == ST_OK ? ST_OK : ST_IO;
}

static int ext4_utimens_impl(ext4_fs_t *fs, const char *path, int nofollow,
			     const struct vfs_utimes *t)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs || !t)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long ino = 0;
	int rr = ext4_resolve_ex(fs, fs->st->cwd_ino, path, !nofollow, &ino, 0);
	if (rr == ST_OK)
		rr = ext4_utimens_ino(fs, ino, t);
	ext4_io_unlock(fs);
	return rr; /* ST_IO is propagated, not turned into ENOENT */
}

static int ext4_get_statfs(ext4_fs_t *fs, unsigned long *f_bsize, unsigned long *f_blocks,
		    unsigned long *f_bfree, unsigned long *f_files,
		    unsigned long *f_ffree, unsigned long *f_namelen,
		    unsigned long *f_type)
{
	if (!fs)
		return -1;
	unsigned long bfree = (unsigned long)fs->sb_copy.s_free_blocks_count_lo;
	if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
		bfree |= (unsigned long)fs->sb_copy.s_free_blocks_count_hi
			 << 32;
	if (f_bsize)
		*f_bsize = fs->block_size;
	if (f_blocks)
		*f_blocks = fs->blocks_count;
	if (f_bfree)
		*f_bfree = bfree;
	if (f_files)
		*f_files = fs->inodes_count;
	if (f_ffree)
		*f_ffree = fs->sb_copy.s_free_inodes_count;
	if (f_namelen)
		*f_namelen = 255;
	if (f_type)
		*f_type = EXT4_SUPER_MAGIC;
	return 0;
}

/* ===================================================================
 * Symlinks, hard links, chmod/chown, lstat.
 * =================================================================== */

static int ext4_symlink(ext4_fs_t *fs, const char *target, const char *linkpath)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!target || !linkpath || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long parent = 0;
	char name[256];
	if (ext4_resolve_parent(fs, linkpath, &parent, name, sizeof(name)) !=
	    ST_OK) {
		ext4_io_unlock(fs);
		return ST_NOT_FOUND;
	}
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned long existing;
	if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK) {
		ext4_io_unlock(fs);
		return ST_EXISTS;
	}
	unsigned tlen = 0;
	while (target[tlen])
		tlen++;
	if (tlen >= fs->block_size) {
		ext4_io_unlock(fs);
		return ST_INVALID;
	}

	unsigned puid = 0, pgid = 0, pmode = 0;
	ext4_inode pin;
	if (ext4_read_inode_loc(fs, parent, &pin, 0, 0) == ST_OK) {
		puid = ext4_inode_uid(&pin);
		pgid = ext4_inode_gid(&pin);
		pmode = pin.i_mode;
	}
	unsigned nuid = puid, ngid = pgid, nmode = S_IFLNK | 0777;
	ext4_init_owner(puid, pgid, pmode, 0, &nuid, &ngid, &nmode);

	unsigned long nino = ext4_alloc_inode(fs, parent, 0);
	if (nino == 0) {
		ext4_io_unlock(fs);
		return ST_NOMEM;
	}
	ext4_inode in;
	mm_memset(&in, 0, sizeof(in));
	in.i_mode = (uint16_t)nmode;
	ext4_inode_set_uid(&in, (uint32_t)nuid);
	ext4_inode_set_gid(&in, (uint32_t)ngid);
	in.i_links_count = 1;
	in.i_size_lo = tlen;
	uint32_t now = (uint32_t)timer_get_epoch();
	ext4_stamp(fs, &in, EXT4_T_ATIME | EXT4_T_MTIME | EXT4_T_CTIME,
		   (int64_t)now, 0);
	if (fs->inode_size > 128)
		in.i_extra_isize = 32;

	if (tlen < 60) { /* fast symlink — inline target  */
		mm_memcpy(in.i_block, target, tlen);
		ext4_inode_cache_flush(fs);
		ext4_write_inode_new(
			fs, nino,
			&in); /* fresh inode: clear stale ibody xattrs */
	} else { /* slow symlink — one data block  */
		in.i_flags = EXT4_INODE_EXTENTS_FL;
		ext4_extent_header *eh = (ext4_extent_header *)in.i_block;
		eh->eh_magic = EXT4_EXT_MAGIC;
		eh->eh_max = 4;
		if (ext4_alloc_blocks_for_file(fs, nino, &in, 0, 1) != 1) {
			ext4_abandon_new_inode(fs, nino, 0);
			ext4_flush_meta(fs);
			ext4_io_unlock(fs);
			return ST_NOMEM;
		}
		ext4_extent *ex = (ext4_extent *)(eh + 1);
		unsigned long pbn = (unsigned long)ex[0].ee_start_lo |
				    ((unsigned long)ex[0].ee_start_hi << 32);
		uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
		if (!blk) {
			ext4_free_blocks_from(fs, nino, &in, 0);
			ext4_abandon_new_inode(fs, nino, 0);
			ext4_flush_meta(fs);
			ext4_io_unlock(fs);
			return ST_NOMEM;
		}
		mm_memset(blk, 0, fs->block_size);
		mm_memcpy(blk, target, tlen);
		ext4_write_sectors(fs->bdev,
				   fs->part_lba_offset +
					   pbn * fs->sectors_per_block,
				   fs->sectors_per_block, blk);
		kfree(blk);
		ext4_inode_cache_flush(fs);
		ext4_write_inode_new(
			fs, nino,
			&in); /* fresh inode: clear stale ibody xattrs */
	}
	if (ext4_dir_add(fs, parent, name, nl, nino, EXT4_FT_SYMLINK) !=
	    ST_OK) {
		ext4_abandon_new_inode(fs, nino, 0);
		ext4_flush_meta(fs);
		ext4_io_unlock(fs);
		return ST_IO;
	}
	ext4_inode_cache_flush(fs);
	ext4_flush_meta(fs);
	ext4_io_unlock(fs);
	return ST_OK;
}

static int ext4_readlink(ext4_fs_t *fs, const char *path, char *buf, unsigned long bufsiz)
{
	if (!path || !buf || !fs || bufsiz == 0)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	unsigned long ino;
	if (ext4_resolve_ex(fs, fs->st->cwd_ino, path, 0, &ino, 0) != ST_OK) {
		ext4_meta_runlock(fs);
		return ST_NOT_FOUND;
	}
	ext4_inode in;
	if (!ext4_get_inode_cached(fs, ino, &in)) {
		ext4_meta_runlock(fs);
		return ST_IO;
	}
	if ((in.i_mode & S_IFMT) != S_IFLNK) {
		ext4_meta_runlock(fs);
		return ST_INVALID;
	}
	char tmp[256];
	int tl = ext4_read_symlink_target(fs, ino, &in, tmp, sizeof(tmp));
	ext4_meta_runlock(fs);
	if (tl < 0)
		return ST_IO;
	unsigned long n = (unsigned long)tl;
	if (n > bufsiz)
		n = bufsiz; /* readlink truncates, no NUL     */
	mm_memcpy(buf, tmp, n);
	return (int)n;
}

/* A hard link `newpath' to what `oldpath' names -- with `follow', to the
 * target of a final symlink, else to the symlink itself (link()'s and
 * linkat()'s default). */
static int ext4_link(ext4_fs_t *fs, const char *oldpath, const char *newpath,
		     int follow)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!oldpath || !newpath || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long src;
	if (ext4_resolve_ex(fs, fs->st->cwd_ino, oldpath, follow ? 1 : 0, &src,
			    0) != ST_OK) {
		ext4_io_unlock(fs);
		return ST_NOT_FOUND;
	}
	ext4_inode sin;
	if (ext4_read_inode_loc(fs, src, &sin, 0, 0) != ST_OK) {
		ext4_io_unlock(fs);
		return ST_IO;
	}
	if ((sin.i_mode & S_IFMT) == S_IFDIR) {
		ext4_io_unlock(fs);
		return ST_INVALID;
	}
	unsigned long parent;
	char name[256];
	if (ext4_resolve_parent(fs, newpath, &parent, name, sizeof(name)) !=
	    ST_OK) {
		ext4_io_unlock(fs);
		return ST_NOT_FOUND;
	}
	unsigned nl = 0;
	while (name[nl])
		nl++;
	unsigned long existing;
	if (ext4_dir_lookup(fs, parent, name, nl, &existing, 0) == ST_OK) {
		ext4_io_unlock(fs);
		return ST_EXISTS;
	}
	unsigned ft = ext4_mode_to_ft(sin.i_mode);
	if (ext4_dir_add(fs, parent, name, nl, src, ft) != ST_OK) {
		ext4_io_unlock(fs);
		return ST_IO;
	}
	sin.i_links_count++;
	ext4_stamp_now(fs, &sin, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	ext4_write_inode_struct(fs, src, &sin);
	ext4_flush_meta(fs);
	ext4_io_unlock(fs);
	return ST_OK;
}

static int ext4_set_mode_locked(ext4_fs_t *fs, unsigned long ino, unsigned mode)
{
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	in.i_mode = (uint16_t)((in.i_mode & S_IFMT) | (mode & 07777));
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	/* A mode change can re-add a set-id bit: invalidate the per-inode strip
     * hint (S_NOSEC analog) for every handle, so the next non-root modify
     * re-evaluates.  Peek the cache without taking a ref. */
	ic_inode_t *ic = icache_lookup(&fs->sb, EXT4_BID_ENC(ino, 0));
	if (ic)
		ic->flags &= ~(uint32_t)IC_SETID_CLEAN;
	return ext4_write_inode_struct(fs, ino, &in) == ST_OK ? ST_OK : ST_IO;
}

static int ext4_set_owner_locked(ext4_fs_t *fs, unsigned long ino, int uid,
				 int gid)
{
	ext4_inode in;
	if (ext4_read_inode_loc(fs, ino, &in, 0, 0) != ST_OK)
		return ST_IO;
	/* -1 leaves a field as it is; every other value is an id, those
	 * above 2^31 included (the full 32 bits are kept). */
	if (uid != -1)
		ext4_inode_set_uid(&in, (uint32_t)uid);
	if (gid != -1)
		ext4_inode_set_gid(&in, (uint32_t)gid);
	ext4_stamp_now(fs, &in, EXT4_T_CTIME);
	ext4_inode_cache_flush(fs);
	return ext4_write_inode_struct(fs, ino, &in) == ST_OK ? ST_OK : ST_IO;
}

static int ext4_chmod(ext4_fs_t *fs, const char *path, unsigned mode)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_resolve(fs, path, &ino);
	if (r == ST_OK)
		r = ext4_set_mode_locked(fs, ino, mode);
	ext4_io_unlock(fs);
	return r;
}

static int ext4_chown(ext4_fs_t *fs, const char *path, int uid, int gid,
		      int nofollow)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!path || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_resolve_ex(fs, fs->st->cwd_ino, path, !nofollow, &ino, 0);
	if (r == ST_OK)
		r = ext4_set_owner_locked(fs, ino, uid, gid);
	ext4_io_unlock(fs);
	return r;
}

/* fd-based fchmod/fchown: chmod/chown by inode. */
static int ext4_fchmod_ino(ext4_fs_t *fs, unsigned long ino, unsigned mode)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!ino || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_set_mode_locked(fs, ino, mode);
	ext4_io_unlock(fs);
	return r;
}

static int ext4_fchown_ino(ext4_fs_t *fs, unsigned long ino, int uid, int gid)
{
	if (ext4_is_ro(fs))
		return ST_ROFS;
	if (!ino || !fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_set_owner_locked(fs, ino, uid, gid);
	ext4_io_unlock(fs);
	return r;
}

static int ext4_lstat(ext4_fs_t *fs, const char *path, struct kstat *st)
{
	if (!path || !st || !fs)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	unsigned long ino;
	int r = ext4_resolve_ex(fs, fs->st->cwd_ino, path, 0, &ino,
				0); /* no follow final */
	if (r == ST_OK)
		r = ext4_stat_fill(fs, ino, st);
	ext4_meta_runlock(fs);
	return r;
}

/* The instance a VFS call is for. */
static inline ext4_fs_t *ext4_fs_of(vfs_mount_t *m)
{
	return m ? (ext4_fs_t *)m->fs_data : NULL;
}

static int ext4_fsync(vfs_file_t *f)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_io_lock(fs);
	if (!ef->is_dir) {
		pagecache_flush_file(&fs->sb, EXT4_BID_ENC(ef->ino, 0));
		if (ef->inode)
			icache_flush((ic_inode_t *)ef->inode);
	}
	/* fsync means "this file is on the device when I return", and a size
	 * held only in memory is part of what that promises.  Everything
	 * pending is stored, not just this file's: the set is small, and one
	 * pass is cheaper than working out which of them matters. */
	ext4_di_flush(fs);
	ext4_flush_meta(fs);
	/* Merge this op into the batch, then durably commit + clean the journal: an
     * explicit sync is a consistency point, so a reboot after it must not replay.
     * journal_clean flushes the batch (one log commit + checkpoint) and syncs. */
	ext4_txn_flush(fs);
	ext4_journal_clean(fs);
	/* Write-back defers data-write I/O errors past write(); surface them here. */
	int werr = fs->st->wb_err;
	fs->st->wb_err = 0;
	ext4_io_unlock(fs);
	return werr ? -EIO : 0;
}

/* Whole-filesystem sync (the sync(2) op, not tied to a file): flush deferred
 * metadata, commit any in-flight transaction, push the device, then mark the
 * journal clean so a reboot right after sync() does not replay.  Mirrors
 * ext4_fsync minus the per-file pagecache/icache flush. */
static int ext4_sync_fs(ext4_fs_t *fs)
{
	if (!fs)
		return 0;
	/* Mounted without replaying its journal (read-only): there is
	 * nothing of ours to store, and storing the superblock would drop
	 * the flag that says the journal still needs replaying. */
	if (fs->mnt_opts & EXT4_MNT_NOLOAD)
		return 0;
	ext4_io_lock(fs);
	ext4_resync_gd_from_bitmaps(
		fs); /* GD free counts/csums <- bitmaps (ground truth) */
	/* Sizes and timestamps held in memory are part of what sync() is being
	 * asked to make durable. */
	ext4_di_flush(fs);
	ext4_flush_meta(fs);
	ext4_txn_flush(fs);
	ext4_journal_clean(fs);
	ext4_io_unlock(fs);
	return 0;
}

static int ext4_sync_op(vfs_mount_t *m)
{
	return ext4_sync_fs(ext4_fs_of(m));
}

/* ---- Locked wrappers (serialise via the reentrant sleeping mutex) ---- */
/* ===================================================================
 * VFS entry points — the locking happens HERE, per the hierarchy
 * (per-inode → metadata → block-group).  Read-only ops take the metadata
 * lock SHARED and run concurrently; mutators take it EXCLUSIVE (the old
 * global-mutex semantics, transaction hooks included).  File-content ops
 * additionally take the file's inode lock so data reads (which transfer
 * with NO metadata lock) can never overlap a free/rewrite of the same
 * file's blocks.  Every lock is the instance's own: two mounted ext4
 * filesystems never wait for each other.
 * =================================================================== */

/* Fence helper for path-named ops that FREE file data (unlink, rename
 * over an existing file, O_TRUNC open): resolve the victim under the
 * shared lock, take its inode lock exclusive, then re-check the name
 * still maps to the same inode under the exclusive metadata lock (it may
 * have been renamed/replaced while unlocked).  Returns 1 with BOTH locks
 * held (*ino_out set) when fenced, 0 with NO locks held when the path
 * does not currently resolve (or keeps racing — caller falls back to the
 * plain exclusive path, where the data-free is safe anyway because the
 * name no longer reaches a file a reader could have mapped). */
static int ext4_fence_path_excl(ext4_fs_t *fs, const char *path, int follow,
				unsigned long *ino_out)
{
	if (!fs)
		return 0;
	for (int tries = 0; tries < 4; tries++) {
		unsigned long ino = 0;
		ext4_meta_rlock(fs);
		int rr = ext4_resolve_ex(fs, fs->st->cwd_ino, path, follow,
					 &ino, 0);
		ext4_meta_runlock(fs);
		if (rr != ST_OK || ino == 0)
			return 0;
		ext4_ilock_excl(fs, ino);
		ext4_io_lock(fs);
		unsigned long ino2 = 0;
		int rr2 = ext4_resolve_ex(fs, fs->st->cwd_ino, path, follow,
					  &ino2, 0);
		if (rr2 == ST_OK && ino2 == ino) {
			*ino_out = ino;
			return 1; /* both locks held */
		}
		ext4_io_unlock(fs);
		ext4_iunlock_excl(fs, ino);
		/* raced with a rename/unlink of the name — retry */
	}
	return 0;
}

static int ext4_open_locked(ext4_fs_t *fs, const char *path, int flags,
			    unsigned cmode, vfs_file_t **out)
{
	if (!fs)
		return ST_INVALID;
	if (flags & (O_CREAT | O_TRUNC)) {
		/* A truncating open frees the file's data blocks: fence
		 * in-flight readers via the inode lock when the target
		 * already exists. */
		if (flags & O_TRUNC) {
			unsigned long ino = 0;
			if (ext4_fence_path_excl(fs, path, 1, &ino)) {
				int r = ext4_open_impl(fs, path, flags, cmode,
						       out);
				ext4_io_unlock(fs);
				ext4_iunlock_excl(fs, ino);
				return r;
			}
		}
		ext4_io_lock(fs);
		int r = ext4_open_impl(fs, path, flags, cmode, out);
		ext4_io_unlock(fs);
		return r;
	}
	ext4_meta_rlock(fs);
	int r = ext4_open_impl(fs, path, flags, cmode, out);
	ext4_meta_runlock(fs);
	return r;
}

/* ->open: no caller-supplied mode.  0666 is the POSIX default for creat(). */
static int ext4_open(vfs_mount_t *m, const char *path, int flags,
		     vfs_file_t **out)
{
	return ext4_open_locked(ext4_fs_of(m), path, flags, 0666, out);
}

/* ->open_mode: `mode` is already umask-adjusted by the syscall layer. */
static int ext4_open_mode(vfs_mount_t *m, const char *path, int flags,
			  unsigned int mode, vfs_file_t **out)
{
	return ext4_open_locked(ext4_fs_of(m), path, flags, mode, out);
}

static int ext4_stat_vfs(vfs_mount_t *m, const char *path, struct kstat *st)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	int r = ext4_stat_vfs_impl(fs, path, st);
	ext4_meta_runlock(fs);
	return r;
}
static long ext4_read(vfs_file_t *f, void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	unsigned long ino = ef->ino;
	ext4_ilock_shared(fs, ino);
	/* Read-your-writes for the data write-back buffer: if it holds THIS
	 * file's blocks, flush them under the exclusive lock first.  It
	 * cannot refill for this inode afterwards — a writer needs the inode
	 * lock exclusive, which we hold shared. */
	ext4_meta_rlock(fs);
	int wb_hit = (fs->st->wb_len && fs->st->wb_ino == ino);
	ext4_meta_runlock(fs);
	if (wb_hit) {
		ext4_io_lock(fs);
		ext4_wb_flush(fs);
		ext4_io_unlock(fs);
	}
	/* Hold the metadata lock SHARED across the read itself.  ext4_read_impl
	 * reads metadata (the inode via ext4_get_inode_cached, extent blocks via
	 * ext4_block_map), and ext4_read_block's read-your-writes lookups are
	 * documented to require this lock at least shared — it is what keeps
	 * the transaction and checkpoint sets stable while they are consulted,
	 * and what serialises the mbc population against a concurrent writer.
	 * Without it, a reader can
	 * fetch a block from disk, a writer can then checkpoint the new content
	 * (refreshing the mbc), and the reader finishes by inserting its now-
	 * stale copy over the fresh one — after which every later read of that
	 * block sees pre-write content, e.g. setxattr succeeding and the next
	 * getxattr returning ENODATA.  Every other reader (stat, getxattr,
	 * listxattr) already holds it this way; this path only took it for the
	 * write-back peek above and dropped it again.
	 *
	 * Taken AFTER the wb flush above: that needs the lock exclusive, so
	 * holding it shared across it would self-deadlock. */
	ext4_meta_rlock(fs);
	long r = ext4_read_impl(f, buf, bytes);
	ext4_meta_runlock(fs);
	ext4_iunlock_shared(fs, ino);
	return r;
}
/* Read at an explicit offset, leaving the descriptor's position alone.
 *
 * Same locking as ext4_read above -- the reasons are identical and written
 * out there -- with the position and the readahead state supplied by the
 * caller instead of taken from the handle. */
static long ext4_read_at(vfs_file_t *f, void *buf, long bytes, long off)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	pc_readahead_t ra;

	if (!ef || !ef->fs || off < 0)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	unsigned long ino = ef->ino;

	ra.last_page_index = 0;
	ra.sequential_count = 0;
	ra.ra_pages = 0;

	ext4_ilock_shared(fs, ino);
	ext4_meta_rlock(fs);
	int wb_hit = (fs->st->wb_len && fs->st->wb_ino == ino);
	ext4_meta_runlock(fs);
	if (wb_hit) {
		ext4_io_lock(fs);
		ext4_wb_flush(fs);
		ext4_io_unlock(fs);
	}
	ext4_meta_rlock(fs);
	long r = ext4_read_at_impl(f, buf, bytes, (unsigned long)off, &ra);
	ext4_meta_runlock(fs);
	ext4_iunlock_shared(fs, ino);
	return r;
}

static long ext4_write(vfs_file_t *f, const void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_ilock_excl(fs, ef->ino);
	ext4_io_lock(fs);
	long r = ext4_write_impl_at(f, buf, bytes, &ef->pos);
	ext4_io_unlock(fs);
	ext4_iunlock_excl(fs, ef->ino);
	return r;
}
/* pwrite: the same at a position of the call's own; the description's is
 * neither read nor moved (ext4_seek touches it without any lock, so it may
 * not even be borrowed). */
static long ext4_write_at(vfs_file_t *f, const void *buf, long bytes, long off)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	unsigned long pos;

	if (!ef || !ef->fs || off < 0)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	pos = (unsigned long)off;
	ext4_ilock_excl(fs, ef->ino);
	ext4_io_lock(fs);
	long r = ext4_write_impl_at(f, buf, bytes, &pos);
	ext4_io_unlock(fs);
	ext4_iunlock_excl(fs, ef->ino);
	return r;
}
static long ext4_seek(vfs_file_t *f, long offset, int whence)
{
	/* Touches only per-open-file fields (pos; size is a single-word
	 * read) — no filesystem lock needed. */
	return ext4_seek_impl(f, offset, whence);
}
static long ext4_readdir(vfs_file_t *f, void *buf, long bytes)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_meta_rlock(fs);
	long r = ext4_readdir_impl(f, buf, bytes);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_truncate(vfs_file_t *f, unsigned long size)
{
	ext4_file_t *ef = f ? (ext4_file_t *)f->fs_private : 0;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_ilock_excl(fs, ef->ino);
	ext4_io_lock(fs);
	int r = ext4_truncate_impl(f, size);
	ext4_io_unlock(fs);
	ext4_iunlock_excl(fs, ef->ino);
	return r;
}
static int ext4_unlink(vfs_mount_t *m, const char *path)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	unsigned long ino = 0;
	if (!fs)
		return ST_INVALID;
	if (ext4_fence_path_excl(fs, path, 0, &ino)) {
		int r = ext4_unlink_impl(fs, path);
		ext4_io_unlock(fs);
		ext4_iunlock_excl(fs, ino);
		return r;
	}
	ext4_io_lock(fs);
	int r = ext4_unlink_impl(fs, path);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_rename(vfs_mount_t *m, const char *o, const char *n)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	/* Only an existing DESTINATION file has its data freed (overwrite);
	 * fence it.  The source inode's data is untouched by rename. */
	unsigned long dst = 0;
	if (ext4_fence_path_excl(fs, n, 0, &dst)) {
		int r = ext4_rename_impl(fs, o, n);
		ext4_io_unlock(fs);
		ext4_iunlock_excl(fs, dst);
		return r;
	}
	ext4_io_lock(fs);
	int r = ext4_rename_impl(fs, o, n);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_mkdir(vfs_mount_t *m, const char *path, unsigned int mode)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_mkdir_impl(fs, path, mode);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_mknod(vfs_mount_t *m, const char *path, unsigned int mode,
		      uint64_t dev)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_mknod_impl(fs, path, mode, dev);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_rmdir(vfs_mount_t *m, const char *path)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_io_lock(fs);
	int r = ext4_rmdir_impl(fs, path);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_chdir(vfs_mount_t *m, const char *path)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	int r = ext4_chdir_impl(fs, path);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_close(vfs_file_t *f)
{
	/* icache_unref locks internally; nothing else is shared. */
	return ext4_close_impl(f);
}

static int ext4_release_locks_for_task(vfs_mount_t *m, uint64_t task_id)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !fs->st)
		return 0;
	int released = ext4_io_release_if_owner(fs, task_id);
	if (fs->bdev && fs->bdev->release_locks_for_task) {
		released |= fs->bdev->release_locks_for_task(
			(block_device_t *)fs->bdev, task_id);
	}
	return released;
}

/* ---- Path ops that change an inode's attributes ---- */
static int ext4_symlink_op(vfs_mount_t *m, const char *target,
			   const char *linkpath)
{
	return ext4_symlink(ext4_fs_of(m), target, linkpath);
}
static int ext4_readlink_op(vfs_mount_t *m, const char *path, char *buf,
			    unsigned long bufsz)
{
	return ext4_readlink(ext4_fs_of(m), path, buf, bufsz);
}
static int ext4_link_op(vfs_mount_t *m, const char *oldpath,
			const char *newpath, int follow)
{
	return ext4_link(ext4_fs_of(m), oldpath, newpath, follow);
}
static int ext4_chmod_op(vfs_mount_t *m, const char *path, unsigned int mode)
{
	return ext4_chmod(ext4_fs_of(m), path, mode);
}
static int ext4_chown_op(vfs_mount_t *m, const char *path, int uid, int gid,
			 int nofollow)
{
	return ext4_chown(ext4_fs_of(m), path, uid, gid, nofollow);
}
static int ext4_lstat_op(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return ext4_lstat(ext4_fs_of(m), path, st);
}
static int ext4_utimens_op(vfs_mount_t *m, const char *path, int nofollow,
			   const struct vfs_utimes *t)
{
	return ext4_utimens_impl(ext4_fs_of(m), path, nofollow, t);
}
static int ext4_futimens_op(vfs_file_t *f, const struct vfs_utimes *t)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs || !t)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	ext4_io_lock(fs);
	int r = ext4_utimens_ino(fs, ef->ino, t);
	ext4_io_unlock(fs);
	return r;
}

/* vfs_ops adapters: address fchmod/fchown by open handle and fill the generic
 * statfs struct, so the syscall layer never calls an ext4_* symbol directly. */
static int ext4_fchmod_op(vfs_file_t *f, unsigned int mode)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs || !ef->ino)
		return ST_OK; /* not an ext4 handle: no-op */
	return ext4_fchmod_ino(ef->fs, ef->ino, mode);
}
static int ext4_fchown_op(vfs_file_t *f, int uid, int gid)
{
	ext4_file_t *ef = (ext4_file_t *)(f ? f->fs_private : 0);
	if (!ef || !ef->fs || !ef->ino)
		return ST_OK;
	return ext4_fchown_ino(ef->fs, ef->ino, uid, gid);
}
static int ext4_statfs_op(vfs_mount_t *m, struct vfs_statfs *out)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!out || !fs)
		return ST_INVALID;
	unsigned long bs, blk, bf, fi, ff, nl, ty;
	if (ext4_get_statfs(fs, &bs, &blk, &bf, &fi, &ff, &nl, &ty) != 0)
		return ST_IO;
	out->f_type = ty;
	out->f_bsize = bs;
	out->f_frsize = bs;
	out->f_blocks = blk;
	out->f_bfree = bf;
	out->f_bavail = bf;
	out->f_files = fi;
	out->f_ffree = ff;
	out->f_fsid = 0;
	out->f_namelen = nl;
	return ST_OK;
}

/* Fill a kstat for an open ext4 handle (real mode/uid/gid) — backs the fd-based
 * permission checks (fchmod/fchown ownership). */
static int ext4_fstat_op(vfs_file_t *f, struct kstat *st)
{
	if (!f || !st)
		return ST_INVALID;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	if (!ef || !ef->fs)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_meta_rlock(fs);
	int r = ext4_stat_fill(fs, ef->ino, st);
	ext4_meta_runlock(fs);
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
         * here if icache_get(&fs->sb, ) failed at open (OOM): report "not clean" so the
         * strip still runs every modify (it reads the mode directly) — safe,
         * just unamortised.  Directories/special files have no inode by design
         * and must never be stripped (e.g. a set-group-ID dir), so: clean. */
		if (mark)
			return 1; /* nowhere to record it */
		return (ef && !ef->is_dir) ? 0 : 1;
	}
	if (mark) {
		ic->flags |= (uint32_t)IC_SETID_CLEAN;
		return 1;
	}
	return (ic->flags & IC_SETID_CLEAN) ? 1 : 0;
}

/* ---- xattr VFS ops ----  Each takes the I/O lock (which begins a journalled
 * transaction); the set/remove paths refuse cleanly on a read-only/errored fs.
 * The full attribute name is parsed into a namespace index + suffix here. */
static int ext4_xattr_resolve(ext4_fs_t *fs, const char *path, int nofollow,
			      unsigned long *ino)
{
	return ext4_resolve_ex(fs, fs->st->cwd_ino, path, !nofollow, ino, 0);
}
static int ext4_getxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			    const char *name, void *val, unsigned long size)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path || !name)
		return ST_INVALID;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_meta_rlock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_get_ino(fs, ino, idx, suf, slen, val, size);
	ext4_meta_runlock(fs);
	return r;
}
/* Fetch by already-resolved inode number, skipping the path walk (used by the
 * permission code, which has the inode from its preceding stat). */
static int ext4_getxattr_ino_op(vfs_mount_t *m, unsigned long ino,
				const char *name, void *val, unsigned long size)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !name || ino == 0)
		return ST_INVALID;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_meta_rlock(fs);
	int r = ext4_xattr_get_ino(fs, ino, idx, suf, slen, val, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_setxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			    const char *name, const void *val,
			    unsigned long size, int flags)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path || !name)
		return ST_INVALID;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_UNSUPPORTED; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_set_ino(fs, ino, idx, suf, slen, val, size,
				       flags, 0);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_listxattr_op(vfs_mount_t *m, const char *path, int nofollow,
			     char *list, unsigned long size)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path)
		return ST_INVALID;
	ext4_meta_rlock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_list_ino(fs, ino, list, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_removexattr_op(vfs_mount_t *m, const char *path, int nofollow,
			       const char *name)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs || !path || !name)
		return ST_INVALID;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	unsigned long ino;
	int r = ext4_xattr_resolve(fs, path, nofollow, &ino);
	if (r == ST_OK)
		r = ext4_xattr_set_ino(fs, ino, idx, suf, slen, 0, 0, 0, 1);
	ext4_io_unlock(fs);
	return r;
}
static ext4_file_t *ext4_ef(vfs_file_t *f)
{
	if (!f || f->ops != &ext4_vfs_ops || !f->fs_private)
		return 0;
	ext4_file_t *ef = (ext4_file_t *)f->fs_private;
	return ef->fs ? ef : 0;
}
static int ext4_fgetxattr_op(vfs_file_t *f, const char *name, void *val,
			     unsigned long size)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef || !name)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_meta_rlock(fs);
	int r = ext4_xattr_get_ino(fs, ef->ino, idx, suf, slen, val, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_fsetxattr_op(vfs_file_t *f, const char *name, const void *val,
			     unsigned long size, int flags)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef || !name)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_UNSUPPORTED; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	int r = ext4_xattr_set_ino(fs, ef->ino, idx, suf, slen, val, size,
				   flags, 0);
	ext4_io_unlock(fs);
	return r;
}
static int ext4_flistxattr_op(vfs_file_t *f, char *list, unsigned long size)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	ext4_meta_rlock(fs);
	int r = ext4_xattr_list_ino(fs, ef->ino, list, size);
	ext4_meta_runlock(fs);
	return r;
}
static int ext4_fremovexattr_op(vfs_file_t *f, const char *name)
{
	ext4_file_t *ef = ext4_ef(f);
	if (!ef || !name)
		return ST_INVALID;
	ext4_fs_t *fs = ef->fs;
	if (ext4_is_ro(fs))
		return ST_ROFS;
	const char *suf;
	unsigned slen;
	int idx = ext4_xattr_name_index(name, &suf, &slen);
	if (idx == 0)
		return ST_NODATA; /* slen==0 is valid for POSIX-ACL names */
	ext4_io_lock(fs);
	int r = ext4_xattr_set_ino(fs, ef->ino, idx, suf, slen, 0, 0, 0, 1);
	ext4_io_unlock(fs);
	return r;
}

/* Report the VFS_ATTR_* flags (immutable / append-only) for an inode, so the
 * VFS can veto modifications independently of the rwx mode bits.  ext4 stores
 * these in the on-disk inode's i_flags; this is the filesystem-specific input
 * to the otherwise-generic VFS permission check. */
static int ext4_inode_flags_op(vfs_mount_t *m, unsigned long ino,
			       uint32_t *out_flags)
{
	if (!out_flags)
		return ST_INVALID;
	*out_flags = 0;
	ext4_fs_t *fs = ext4_fs_of(m);
	if (!fs)
		return ST_NO_DEVICE;
	ext4_inode in;
	ext4_meta_rlock(fs);
	int r = ext4_read_inode_loc(fs, ino, &in, 0, 0);
	ext4_meta_runlock(fs);
	if (r != ST_OK)
		return ST_IO;
	if (in.i_flags & EXT4_INODE_IMMUTABLE_FL)
		*out_flags |= VFS_ATTR_IMMUTABLE;
	if (in.i_flags & EXT4_INODE_APPEND_FL)
		*out_flags |= VFS_ATTR_APPEND;
	return ST_OK;
}

static const vfs_ops_t ext4_vfs_ops = {
	.open = ext4_open,
	.stat = ext4_stat_vfs,
	.read = ext4_read,
	.read_at = ext4_read_at,
	.write_at = ext4_write_at,
	.write = ext4_write,
	.seek = ext4_seek,
	.readdir = ext4_readdir,
	.truncate = ext4_truncate,
	.unlink = ext4_unlink,
	.rename = ext4_rename,
	.mkdir = ext4_mkdir,
	.rmdir = ext4_rmdir,
	.chdir = ext4_chdir,
	.close = ext4_close,
	.release_locks_for_task = ext4_release_locks_for_task,
	.fsync = ext4_fsync,
	.lstat = ext4_lstat_op,
	.symlink = ext4_symlink_op,
	.readlink = ext4_readlink_op,
	.link = ext4_link_op,
	.chmod = ext4_chmod_op,
	.chown = ext4_chown_op,
	.fchmod = ext4_fchmod_op,
	.fchown = ext4_fchown_op,
	.utimens = ext4_utimens_op,
	.futimens = ext4_futimens_op,
	.statfs = ext4_statfs_op,
	.fstat = ext4_fstat_op,
	.setid_clean = ext4_setid_clean_op,
	.sync = ext4_sync_op,
	.getxattr = ext4_getxattr_op,
	.setxattr = ext4_setxattr_op,
	.listxattr = ext4_listxattr_op,
	.removexattr = ext4_removexattr_op,
	.fgetxattr = ext4_fgetxattr_op,
	.fsetxattr = ext4_fsetxattr_op,
	.flistxattr = ext4_flistxattr_op,
	.fremovexattr = ext4_fremovexattr_op,
	.getxattr_ino = ext4_getxattr_ino_op,
	.inode_flags = ext4_inode_flags_op,
	.mknod = ext4_mknod,
	.open_mode = ext4_open_mode,
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
	if (sb_sectors < 1)
		sb_sectors = 1;
	uint8_t *raw = (uint8_t *)kalloc(sb_sectors * ss);
	if (!raw)
		return 0;
	int ok = 0;
	if (ext4_read_sectors(bdev, sb_sector, sb_sectors, raw) == ST_OK) {
		ext4_super_block *sb = (ext4_super_block *)raw;
		if (sb->s_magic == EXT4_SUPER_MAGIC)
			ok = 1;
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
	if (!hdr)
		return (unsigned long)-1;
	unsigned long found = (unsigned long)-1;

	/* GPT header is at LBA 1, signature "EFI PART". */
	if (ext4_read_sectors(bdev, 1, 1, hdr) == ST_OK && hdr[0] == 'E' &&
	    hdr[1] == 'F' && hdr[2] == 'I' && hdr[3] == ' ' && hdr[4] == 'P' &&
	    hdr[5] == 'A' && hdr[6] == 'R' && hdr[7] == 'T') {
		uint64_t ent_lba;
		uint32_t num_ent, ent_sz;
		mm_memcpy(&ent_lba, hdr + 72,
			  sizeof(ent_lba)); /* PartitionEntryLBA */
		mm_memcpy(&num_ent, hdr + 80,
			  sizeof(num_ent)); /* NumberOfEntries   */
		mm_memcpy(&ent_sz, hdr + 84,
			  sizeof(ent_sz)); /* SizeOfEntry       */
		if (ent_sz >= 56 && ent_sz <= 1024 && num_ent &&
		    num_ent <= 256) {
			unsigned long bytes = (unsigned long)num_ent * ent_sz;
			unsigned long secs = (bytes + ss - 1) / ss;
			if (secs > 64)
				secs = 64; /* bound the read    */
			uint8_t *arr = (uint8_t *)kalloc(secs * ss);
			if (arr) {
				if (ext4_read_sectors(bdev, ent_lba, secs,
						      arr) == ST_OK) {
					unsigned long max_ent =
						(secs * ss) / ent_sz;
					if (max_ent > num_ent)
						max_ent = num_ent;
					for (unsigned long i = 0; i < max_ent;
					     i++) {
						uint8_t *e = arr + i * ent_sz;
						int zero = 1;
						for (int b = 0; b < 16; b++)
							if (e[b]) {
								zero = 0;
								break;
							}
						if (zero)
							continue; /* unused entry      */
						uint64_t start;
						mm_memcpy(
							&start, e + 32,
							sizeof(start)); /* StartingLBA */
						if (start &&
						    ext4_probe_offset(
							    bdev,
							    (unsigned long)
								    start)) {
							found = (unsigned long)
								start;
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
 * Journal replay (jbd2 recovery) — replay-on-mount half.
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
 * and fast-commit are still not specially handled.
 * =================================================================== */

static inline uint16_t be16(uint16_t v)
{
	return __builtin_bswap16(v);
}
static inline uint32_t be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

/* Circular-log advance: log blocks live in [s_first, s_maxlen). */
static unsigned long jlog_advance(unsigned long cur, unsigned long n,
				  unsigned long first, unsigned long maxlen)
{
	unsigned long span = (maxlen > first) ? (maxlen - first) : 0;
	if (span == 0)
		return cur;
	return first + (((cur - first) + n) % span);
}

/* Read journal log block `lbno` (journal-file logical block) into buf. */
static int jlog_read(ext4_fs_t *fs, unsigned long lbno, void *buf)
{
	unsigned long pbn = ext4_block_map(fs, fs->journal_inum, lbno);
	if (pbn == 0)
		return ST_IO; /* the journal file must be fully allocated */
	return ext4_read_block(fs, pbn, buf);
}

/* Write journal log block `lbno` (raw, bypassing the metadata cache and the
 * transaction capture — these go to the on-disk journal, not final FS blocks). */
static int jlog_write(ext4_fs_t *fs, unsigned long lbno, const void *buf)
{
	unsigned long pbn = ext4_block_map(fs, fs->journal_inum, lbno);
	if (pbn == 0)
		return ST_IO;
	return ext4_write_sectors(
		fs->bdev, fs->part_lba_offset + pbn * fs->sectors_per_block,
		fs->sectors_per_block, buf);
}

/* Revoke table: dynamic array of {block, highest-revoked-seq}. */
typedef struct {
	unsigned long block;
	uint32_t seq;
} jrev_ent_t;
typedef struct {
	jrev_ent_t *v;
	unsigned n, cap;
} jrev_tbl_t;

static void jrev_add(jrev_tbl_t *t, unsigned long block, uint32_t seq)
{
	for (unsigned i = 0; i < t->n; i++)
		if (t->v[i].block == block) { /* keep the latest revoke   */
			if (seq > t->v[i].seq)
				t->v[i].seq = seq;
			return;
		}
	if (t->n == t->cap) {
		unsigned ncap = t->cap ? t->cap * 2 : 64;
		jrev_ent_t *nv =
			(jrev_ent_t *)kalloc(ncap * sizeof(jrev_ent_t));
		if (!nv) {
			WARN_ON_ONCE(1);
			return;
		} /* drop: at worst over-replay */
		if (t->v) {
			mm_memcpy(nv, t->v, t->n * sizeof(jrev_ent_t));
			kfree(t->v);
		}
		t->v = nv;
		t->cap = ncap;
	}
	t->v[t->n].block = block;
	t->v[t->n].seq = seq;
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
static uint32_t ext4_jblock_csum(uint32_t seed, uint32_t seq, const void *blk,
				 unsigned bs)
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
	uint32_t raw = *pchk; /* stored be32 csum             */
	*pchk = 0;
	uint32_t calc = ext4_crc32c(seed, blk, bs);
	*pchk = raw; /* restore the buffer           */
	return be32(raw) == calc;
}

/* Stamp the journal SUPERBLOCK checksum (csum v2/v3): s_checksum @0xFC =
 * crc32c(~0, first 1024 bytes with that field zeroed).  Note the ~0 seed (the
 * uuid is inside the summed region), unlike the tag/desc/commit csums.  No-op
 * unless `enabled`.  `jsbbuf` is a full-block buffer. */
static void ext4_jsb_csum_set(uint8_t *jsbbuf, int enabled)
{
	if (!enabled)
		return;
	uint32_t *pc = (uint32_t *)(jsbbuf + JBD2_SB_CSUM_OFF);
	*pc = 0;
	*pc = __builtin_bswap32(
		ext4_crc32c(0xFFFFFFFFu, jsbbuf, JBD2_SB_CSUM_LEN));
}

#define EXT4_JPASS_SCAN 0
#define EXT4_JPASS_REVOKE 1
#define EXT4_JPASS_REPLAY 2

/* One pass over the committed log.  SCAN sets *end_txn (sequence past the last
 * commit); REVOKE/REPLAY are bounded by it.  Returns ST_OK or an I/O error. */
static int ext4_journal_pass(ext4_fs_t *fs, const journal_superblock_t *jsb,
			     int pass, uint32_t *end_txn, jrev_tbl_t *revtbl,
			     unsigned long *out_replayed)
{
	unsigned long first = be32(jsb->s_first);
	unsigned long maxlen = be32(jsb->s_maxlen);
	uint32_t jincompat = be32(jsb->s_feature_incompat);
	int is64 = (jincompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0;
	int csum3 = (jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0;
	int csum2 = (jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) != 0;
	int csum_any = csum2 || csum3; /* descriptor/commit csums    */
	unsigned tag_sz = csum3 ? 16u : (is64 ? 12u : 8u);
	uint32_t jseed = ext4_jcsum_seed(jsb);

	unsigned long next_log = be32(jsb->s_start);
	uint32_t next_seq = be32(jsb->s_sequence);
	unsigned long replayed = 0;
	int rc = ST_OK;

	uint8_t *blk = (uint8_t *)kalloc(fs->block_size);
	uint8_t *data = (uint8_t *)kalloc(fs->block_size);
	if (!blk || !data) {
		if (blk)
			kfree(blk);
		if (data)
			kfree(data);
		return ST_NOMEM;
	}

	for (;;) {
		if (pass != EXT4_JPASS_SCAN && next_seq >= *end_txn)
			break; /* all committed txns done */
		if (jlog_read(fs, next_log, blk) != ST_OK) {
			rc = ST_IO;
			break;
		}
		const journal_header_t *h = (const journal_header_t *)blk;
		if (be32(h->h_magic) != JBD2_MAGIC_NUMBER)
			break; /* end of log     */
		if (be32(h->h_sequence) != next_seq)
			break; /* stale: end     */

		uint32_t bt = be32(h->h_blocktype);
		if (bt == JBD2_DESCRIPTOR_BLOCK) {
			/* csum v2/v3: verify the descriptor TAIL csum before trusting any
             * tag.  A bad tail means a torn/corrupt descriptor — stop here, so
             * the tag walk never advances next_log off garbage. */
			if (csum_any &&
			    !ext4_jdesc_csum_ok(jseed, blk, fs->block_size)) {
				if (pass == EXT4_JPASS_SCAN)
					kprintf("ext4: journal descriptor csum bad at seq %u; "
						"stopping replay there\n",
						next_seq);
				break;
			}
			unsigned off = sizeof(journal_header_t);
			unsigned long data_idx = 0;
			while (off + tag_sz <= fs->block_size) {
				const uint8_t *tag = blk + off;
				uint32_t blocknr =
					be32(*(const uint32_t *)(tag + 0));
				uint32_t flags =
					csum3 ? be32(*(const uint32_t
							       *)(tag +
								  JBD2_TAG3_FLAGS_OFF)) :
						be16(*(const uint16_t *)(tag +
									 6));
				unsigned long target = blocknr;
				if (is64) /* high 32 at +8 (both)*/
					target |=
						((unsigned long)be32(*(
							 const uint32_t
								 *)(tag +
								    JBD2_TAG3_BLKHI_OFF))
						 << 32);

				if (pass == EXT4_JPASS_REPLAY) {
					/* The journalled copy is 1 + data_idx blocks past the
                     * descriptor in the log. */
					unsigned long dl = jlog_advance(
						next_log, 1 + data_idx, first,
						maxlen);
					if (jlog_read(fs, dl, data) != ST_OK) {
						rc = ST_IO;
						goto out;
					}
					/* Per-block TAG csum (csum v2/v3) is over the journalled
                     * (escaped) bytes — verify BEFORE restoring the magic. */
					int tagbad = 0;
					if (csum_any) {
						uint32_t want =
							ext4_jblock_csum(
								jseed, next_seq,
								data,
								fs->block_size);
						int ok =
							csum3 ? (be32(*(const uint32_t
										*)(tag +
										   JBD2_TAG3_CSUM_OFF)) ==
								 want) :
								(be16(*(const uint16_t
										*)(tag +
										   JBD2_TAG_V2_CSUM_OFF)) ==
								 (uint16_t)
									 want);
						if (!ok) {
							kprintf("ext4: journal block tag csum bad (block %lu, "
								"seq %u); not applying that block\n",
								target,
								next_seq);
							tagbad = 1;
						}
					}
					if (flags &
					    JBD2_FLAG_ESCAPE) { /* restore escaped magic */
						uint32_t m = __builtin_bswap32(
							JBD2_MAGIC_NUMBER);
						mm_memcpy(data, &m, 4);
					}
					if (target == 0) {
						/* An otherwise csum-valid descriptor should never
						 * carry a block-0 tag (the writer refuses pbn 0);
						 * log enough context to identify the source. */
						kprintf("ext4: journal seq %u tag %lu (of a %s descriptor at log block %lu) targets block 0 - skipped\n",
							next_seq, data_idx,
							tagbad ? "csum-BAD" :
								 "csum-ok",
							next_log);
					} else if (!tagbad &&
						   !jrev_test(revtbl, target,
							      next_seq)) {
						if (ext4_write_block(fs, target,
								     data) !=
						    ST_OK) {
							rc = ST_IO;
							goto out;
						}
						replayed++;
					}
				}
				data_idx++;
				off += tag_sz;
				if (!(flags & JBD2_FLAG_SAME_UUID))
					off += 16; /* a UUID follows tag  */
				if (flags & JBD2_FLAG_LAST_TAG)
					break;
			}
			next_log = jlog_advance(next_log, 1 + data_idx, first,
						maxlen);
		} else if (bt == JBD2_COMMIT_BLOCK) {
			if (csum_any &&
			    !ext4_jcommit_csum_ok(jseed, blk, fs->block_size)) {
				/* Torn/corrupt commit on a csum journal: this transaction never
                 * completed, so it — and everything after — must NOT be replayed. */
				if (pass == EXT4_JPASS_SCAN)
					kprintf("ext4: journal commit csum bad at seq %u; "
						"stopping replay there\n",
						next_seq);
				break;
			}
			next_seq++; /* transaction boundary */
			if (pass == EXT4_JPASS_SCAN)
				*end_txn = next_seq;
			next_log = jlog_advance(next_log, 1, first, maxlen);
		} else if (bt == JBD2_REVOKE_BLOCK) {
			if (pass == EXT4_JPASS_REVOKE) {
				const jbd2_revoke_header_t *rh =
					(const jbd2_revoke_header_t *)blk;
				unsigned used = be32(rh->r_count);
				if (used > fs->block_size)
					used = fs->block_size;
				unsigned rsz = is64 ? 8u : 4u;
				unsigned p = sizeof(jbd2_revoke_header_t);
				while (p + rsz <= used) {
					unsigned long rb = be32(
						*(const uint32_t *)(blk + p));
					if (is64)
						rb |= ((unsigned long)be32(*(
							       const uint32_t
								       *)(blk +
									  p +
									  4))
						       << 32);
					jrev_add(revtbl, rb, next_seq);
					p += rsz;
				}
			}
			next_log = jlog_advance(next_log, 1, first, maxlen);
		} else {
			break; /* unknown type: end   */
		}
	}
out:
	kfree(blk);
	kfree(data);
	if (out_replayed)
		*out_replayed = replayed;
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
/* Replay the journal.  With `dry', nothing is written: ST_ROFS says the
 * journal holds committed transactions a replay would write, ST_OK that it
 * holds none. */
static int ext4_journal_recover(ext4_fs_t *fs, int *did_work, int dry)
{
	if (did_work)
		*did_work = 0;
	if (fs->journal_inum == 0)
		return ST_OK;
	uint8_t *jblk = (uint8_t *)kalloc(fs->block_size);
	if (!jblk)
		return ST_NOMEM;

	/* The journal superblock is journal log block 0. */
	unsigned long sb_pbn = ext4_block_map(fs, fs->journal_inum, 0);
	if (sb_pbn == 0 || ext4_read_block(fs, sb_pbn, jblk) != ST_OK) {
		kfree(jblk);
		return ST_IO;
	}
	journal_superblock_t *jsb = (journal_superblock_t *)jblk;
	if (be32(jsb->s_header.h_magic) != JBD2_MAGIC_NUMBER) {
		kprintf("ext4: bad journal superblock magic\n");
		kfree(jblk);
		return ST_INVALID;
	}
	uint32_t sbt = be32(jsb->s_header.h_blocktype);
	if (sbt != JBD2_SUPERBLOCK_V1 && sbt != JBD2_SUPERBLOCK_V2) {
		kprintf("ext4: unexpected journal sb blocktype %u\n", sbt);
		kfree(jblk);
		return ST_INVALID;
	}
	if (be32(jsb->s_blocksize) != fs->block_size) {
		kprintf("ext4: journal blocksize %u != fs %u (unsupported)\n",
			be32(jsb->s_blocksize), fs->block_size);
		kfree(jblk);
		return ST_UNSUPPORTED;
	}

	uint32_t jincompat = be32(jsb->s_feature_incompat);
	uint32_t known =
		JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT |
		JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3 |
		JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT |
		JBD2_FEATURE_INCOMPAT_FAST_COMMIT;
	if (jincompat & ~known)
		kprintf("ext4: journal unknown incompat 0x%x; attempting replay anyway\n",
			jincompat & ~known);
	if (jincompat &
	    (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3))
		kprintf("ext4: journal csum-v%d: descriptor/tag/commit csums verified on replay\n",
			(jincompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) ? 3 : 2);
	if (jincompat & (JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT |
			 JBD2_FEATURE_INCOMPAT_FAST_COMMIT))
		kprintf("ext4: journal async/fast-commit not specially handled (tolerant replay)\n");

	/* Always report the journal state at mount so a "did it recover?" question
     * is never a guess.  s_start is the log tail: 0 == clean (nothing to
     * replay), non-zero == a committed-but-uncheckpointed transaction exists. */
	uint32_t jstart = be32(jsb->s_start);
	kprintf("ext4: journal state s_start=%u seq=%u recover_flag=%d\n",
		jstart, be32(jsb->s_sequence),
		(fs->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) ? 1 : 0);
	if (jstart == 0) {
		kfree(jblk);
		return ST_OK; /* clean log: nothing to replay     */
	}
	kprintf("ext4: replaying journal (s_start=%u)...\n", jstart);

	/* Pass 1 — scan to the end of the committed log. */
	uint32_t end_txn = be32(jsb->s_sequence);
	jrev_tbl_t revtbl = { 0, 0, 0 };
	int rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_SCAN, &end_txn, &revtbl,
				   0);
	if (rc != ST_OK) {
		kfree(jblk);
		return rc;
	}
	if (end_txn == be32(jsb->s_sequence)) {
		kprintf("ext4: journal has no committed transactions; nothing to replay\n");
		kfree(jblk);
		return ST_OK;
	}
	if (dry) {
		kfree(jblk);
		return ST_ROFS; /* a replay would write */
	}

	/* Pass 2 — collect revoke records (only if the journal uses them). */
	if (jincompat & JBD2_FEATURE_INCOMPAT_REVOKE) {
		rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_REVOKE, &end_txn,
				       &revtbl, 0);
		if (rc != ST_OK) {
			if (revtbl.v)
				kfree(revtbl.v);
			kfree(jblk);
			return rc;
		}
	}

	/* Pass 3 — replay committed blocks to their final locations. */
	unsigned long replayed = 0;
	rc = ext4_journal_pass(fs, jsb, EXT4_JPASS_REPLAY, &end_txn, &revtbl,
			       &replayed);
	if (revtbl.v)
		kfree(revtbl.v);
	if (rc != ST_OK) {
		kfree(jblk);
		return rc;
	}

	/* Mark the journal empty: s_start = 0, s_sequence = end_txn.  On a csum
     * journal the superblock carries its own checksum (seed ~0 over 1024 bytes),
     * so restamp it after editing or e2fsck will reject the journal sb. */
	jsb->s_start = __builtin_bswap32(0);
	jsb->s_sequence = __builtin_bswap32(end_txn);
	ext4_jsb_csum_set(jblk,
			  (jincompat & (JBD2_FEATURE_INCOMPAT_CSUM_V2 |
					JBD2_FEATURE_INCOMPAT_CSUM_V3)) != 0);
	if (ext4_write_block(fs, sb_pbn, jblk) != ST_OK) {
		kfree(jblk);
		return ST_IO;
	}
	ext4_dev_sync(fs, "replay"); /* persist before RECOVER clears */

	kprintf("ext4: journal replay complete (%lu block(s) recovered, end_txn=%u)\n",
		replayed, end_txn);
	if (did_work && replayed > 0)
		*did_work = 1;
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
 * Called with st->txn inactive, so the writeback goes direct. */
static void ext4_checkpoint(ext4_fs_t *fs)
{
	if (!fs || !fs->j_enabled || !fs->j_sb_buf || !fs->st->epoch_open)
		return;
	int failed = 0;
	for (unsigned i = 0; i < fs->st->ckpt.n; i++) /* 1. pending -> final     */
		if (ext4_write_block_direct(fs, fs->st->ckpt.blk[i], fs->st->ckpt.data[i]) !=
		    ST_OK)
			failed = 1;
	/* The superblock (free counts) rides the batch instead of being written on
     * every allocating op — it becomes durable here with the matching GDT/bitmaps,
     * so the on-disk super and group descriptors are always consistent at a flush
     * (and mount recomputes the free totals from the GDT anyway).  Re-derive the
     * free totals from the (authoritative) GDT first so the super can never drift
     * from the descriptors — the same self-healing the mount path applies. */
	{
		int is64 = (fs->feature_incompat &
			    EXT4_FEATURE_INCOMPAT_64BIT) != 0;
		uint64_t fb = 0;
		uint32_t fi = 0;
		for (unsigned g = 0; g < fs->groups_count; g++) {
			uint32_t gb = fs->gdt[g].bg_free_blocks_count_lo;
			uint32_t gi = fs->gdt[g].bg_free_inodes_count_lo;
			if (is64) {
				gb |= (uint32_t)fs->gdt[g]
					      .bg_free_blocks_count_hi
				      << 16;
				gi |= (uint32_t)fs->gdt[g]
					      .bg_free_inodes_count_hi
				      << 16;
			}
			fb += gb;
			fi += gi;
		}
		fs->sb_copy.s_free_blocks_count_lo = (uint32_t)fb;
		fs->sb_copy.s_free_blocks_count_hi = (uint32_t)(fb >> 32);
		fs->sb_copy.s_free_inodes_count = fi;
	}
	if (ext4_write_super(fs) != ST_OK)
		failed = 1;
	if (ext4_dev_sync(fs, "checkpoint-final") !=
	    ST_OK) /* finals durable BEFORE s_start=0 */
		failed = 1;

	/* If ANY final write or its barrier failed, the on-disk final locations
	 * are an incomplete image of this batch.  Emptying the log (s_start=0) and
	 * dropping the in-memory batch here would be a SILENT, UNRECOVERABLE revert
	 * of every block whose final write failed — e.g. a directory's inode-table
	 * block reverts below its already-persisted grown extent tree / htree leaf,
	 * which is exactly the "/tmp entry vanishes, extent past i_size" corruption.
	 * Instead keep the committed transaction in the journal (so a later
	 * checkpoint, or a mount-time replay, can complete it) and keep st->ckpt (so
	 * read-your-writes still serves the fresh content and no reader observes the
	 * half-written disk).  The next flush re-commits the batch (a superset,
	 * latest-content-wins) from j_first and retries the checkpoint. */
	if (failed) {
		WARN_RATELIMIT(1,
			       "ext4: checkpoint incomplete (device write/flush "
			       "failed) - keeping journal + batch for recovery");
		return;
	}

	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	jsb->s_start = 0; /* 2. empty the log        */
	jsb->s_sequence = __builtin_bswap32(fs->j_sequence);
	ext4_jsb_csum_set(fs->j_sb_buf, fs->j_csum3);
	jlog_write(fs, 0, fs->j_sb_buf);
	ext4_dev_sync(fs, "checkpoint-jsb");

	fs->st->ckpt.n = 0; /* 3. epoch closed         */
	fs->st->epoch_open = 0;
	fs->st->jhead = fs->j_first;
}

/* ===================================================================
 * Journaled writes (ordered mode), deferred-checkpoint circular log.
 *
 * Called from ext4_io_unlock on the outermost release with the lock held.  Each
 * op's buffered metadata (st->txn) is journalled to the log head and made durable
 * with ONE device sync, then recorded in the in-memory epoch pending set
 * (st->ckpt) — it is NOT checkpointed to its final location yet.  Steady-state
 * cost is therefore one sync per op; the 2-sync checkpoint (ext4_checkpoint) is
 * amortised over a whole epoch and runs only when the log/pending set fills, the
 * batch fills, or on fsync/sync.
 *
 * Crash safety: recovery reads s_start (the epoch start, == j_first, made
 * durable by the epoch's first commit sync) and replays every committed txn in
 * sequence order until the chain breaks (torn/absent commit, validated by the
 * csum-v3 commit + descriptor + tag csums).  Because checkpoint is deferred, the
 * committed metadata lives only in the log until then, so reads consult st->ckpt
 * (read-your-writes) — critically, the allocator reads bitmaps from there, so it
 * never double-allocates a block the epoch already used.  Monotonic sequences +
 * never letting an epoch wrap (we checkpoint before the head reaches j_maxlen)
 * mean a stale older epoch left in the log can never extend the new chain.
 * Data blocks are written in place during the op and flushed by the commit sync
 * ("ordered").  No jbd2 revoke records are needed: a journal copy lives only in
 * memory until this function writes it, so a block that is freed simply has its
 * copy dropped from the in-memory batch (ext4_drop_journal_copy) and there is
 * nothing left that could be replayed over the block's next use.
 * =================================================================== */
/* Durably commit the whole in-memory batch (st->ckpt) as ONE journal transaction,
 * then checkpoint it to its final locations and empty the log.  Called lazily —
 * when the batch fills, on fsync/sync/unmount, or when a journalled block is
 * freed — NOT per operation.  This is the single device-sync cost that is now
 * amortised across an entire batch of operations.
 *
 * Crash safety is unchanged in shape from the old per-op commit, just batched:
 * the txn is made durable (commit sync) before its blocks are written to their
 * final homes (checkpoint), which is itself made durable before the log is
 * emptied (s_start=0).  Between flushes the on-disk log is empty, so a crash
 * outside the brief flush window replays nothing. */
/* ---- Ordered data: which inodes this transaction must store first --------
 *
 * Metadata that points at freshly written blocks must not reach the device
 * before the blocks do.  Otherwise a crash in between leaves an inode whose
 * extents name blocks that were never written -- and what is read back is
 * whatever those blocks held for their previous owner, which is somebody
 * else's data.
 *
 * The obvious way to guarantee it is to store EVERY dirty page before every
 * commit.  It is correct, and it is also why an unrelated program doing a
 * small write() could be held up for as long as the whole system's backlog
 * took to write: commits run inside write(), and the bill goes to whoever is
 * standing there.
 *
 * Only the files this transaction actually wrote to need ordering.  So each
 * write records its inode here, and the commit stores exactly those.  A
 * program that wrote nothing pays nothing.
 *
 * Guarded by the filesystem I/O lock, which every writer and the commit both
 * hold exclusively -- no separate lock, and none needed.
 */
#define EXT4_ORDERED_INIT_CAP 64
/* Set when an inode could not be recorded.  The set is then incomplete, so the
 * commit falls back to storing everything -- slower, never wrong. */
/* The array and its count are changed under the I/O lock as described above,
 * and also under this, so that the commit thread can read the set before it
 * takes the I/O lock (ext4_ordered_writeback). */

static void ext4_ordered_add(ext4_fs_t *fs, unsigned long ino)
{
	uint64_t fl;

	if (ino == 0)
		return;

	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	for (unsigned i = 0; i < fs->st->ord_n; i++) {
		if (fs->st->ord_ino[i] == ino) {
			spin_unlock_irqrestore(&fs->st->ord_lock, fl);
			return; /* already recorded */
		}
	}
	if (fs->st->ord_n < fs->st->ord_cap) {
		fs->st->ord_ino[fs->st->ord_n++] = ino;
		spin_unlock_irqrestore(&fs->st->ord_lock, fl);
		return;
	}
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);

	/* Grown outside the spinlock (allocation may sleep).  Nothing else
	 * adds meanwhile: every writer holds the I/O lock exclusively. */
	unsigned ncap = fs->st->ord_cap ? fs->st->ord_cap * 2 : EXT4_ORDERED_INIT_CAP;
	unsigned long *n = (unsigned long *)kalloc(ncap * sizeof(unsigned long));
	unsigned long *old;

	if (!n) {
		/* Cannot record it, so the set no longer describes the
		 * transaction.  Say so; the commit will store everything
		 * rather than miss this inode. */
		fs->st->ord_overflow = 1;
		return;
	}
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	for (unsigned i = 0; i < fs->st->ord_n; i++)
		n[i] = fs->st->ord_ino[i];
	old = fs->st->ord_ino;
	fs->st->ord_ino = n;
	fs->st->ord_cap = ncap;
	fs->st->ord_ino[fs->st->ord_n++] = ino;
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	if (old)
		kfree(old);
}

/* The set as it stands, as cache ids, in a fresh array the caller frees;
 * NULL when it is empty (or there is no memory for the copy). */
static unsigned long *ext4_ordered_snapshot(ext4_fs_t *fs, unsigned *count)
{
	uint64_t fl;
	unsigned want, n = 0;
	unsigned long *copy;

	*count = 0;
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	want = fs->st->ord_n;
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	if (!want)
		return NULL;
	copy = (unsigned long *)kalloc((want + 16) * sizeof(unsigned long));
	if (!copy)
		return NULL;
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	for (unsigned i = 0; i < fs->st->ord_n && n < want + 16; i++)
		copy[n++] = EXT4_BID_ENC(fs->st->ord_ino[i], 0);
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	if (!n) {
		kfree(copy);
		return NULL;
	}
	*count = n;
	return copy;
}

/*
 * Store the data this transaction is responsible for, and forget it.
 * Caller holds the I/O lock.
 *
 * Returns non-zero if anything was written, so the caller knows whether a
 * barrier is needed before the commit.
 */
static int ext4_ordered_flush(ext4_fs_t *fs)
{
	int wrote = 0;
	unsigned n;
	unsigned long *ids;
	uint64_t fl;

	might_sleep();

	if (fs->st->ord_overflow) {
		/* The record is incomplete -- store everything instead of
		 * guessing which inode was missed. */
		WARN_ON_ONCE(1);
		wrote = pagecache_flush_sb(&fs->sb);
		fs->st->ord_overflow = 0;
		spin_lock_irqsave(&fs->st->ord_lock, &fl);
		fs->st->ord_n = 0;
		spin_unlock_irqrestore(&fs->st->ord_lock, fl);
		return wrote;
	}

	/* One pass over the dirty list for the whole set, not one per file. */
	ids = ext4_ordered_snapshot(fs, &n);
	if (ids) {
		wrote = pagecache_flush_fileset(&fs->sb, ids, n);
		kfree(ids);
	} else if (fs->st->ord_n) {
		/* no memory for the copy: store everything */
		wrote = pagecache_flush_sb(&fs->sb);
	}
	spin_lock_irqsave(&fs->st->ord_lock, &fl);
	fs->st->ord_n = 0;
	spin_unlock_irqrestore(&fs->st->ord_lock, fl);
	return wrote;
}

/*
 * The same data, stored by the commit thread BEFORE it takes the I/O lock.
 *
 * The commit holds the I/O lock -- the metadata lock, exclusively -- from the
 * moment it starts until the transaction is on the device, and every lookup,
 * stat and page-in on the filesystem waits for it meanwhile.  Storing the
 * transaction's file data inside that hold made it as long as the data took to
 * reach the device: after a browser had cached a few hundred megabytes of
 * video, tens of seconds on a USB stick, with every program that needed a page
 * from disk frozen.  Written here first, a batch at a time (each batch takes
 * the lock only around its own device writes, so readers get their turn in
 * between), the commit is left with only what was written in the last moment.
 * The order the commit promises -- data before the metadata that refers to it
 * -- is kept: the commit still stores whatever is dirty, under the lock.
 */
static void ext4_ordered_writeback(ext4_fs_t *fs)
{
	unsigned n;
	unsigned long *ids = ext4_ordered_snapshot(fs, &n);

	if (!ids)
		return;
	(void)pagecache_flush_fileset(&fs->sb, ids, n);
	kfree(ids);
}

static void ext4_journal_flush(ext4_fs_t *fs)
{
	if (!fs || !fs->j_enabled || !fs->j_sb_buf)
		return;
	/* data=ordered: write the buffered file data to its final home AND make it
     * durable BEFORE this transaction commits the metadata that references those
     * blocks, so a crash can never expose stale block contents through committed
     * metadata.  One extra sync here is cheap — this runs per batch, not per op. */
	{
		/* Store the data this transaction wrote before the metadata
		 * describing it -- and only that data.  Which inodes those are
		 * was recorded as the writes happened; see ext4_ordered_add.
		 *
		 * Taking the filesystem I/O lock again from here is safe: it is
		 * recursive and only the outermost release commits, so this
		 * cannot re-enter the journal. */
		int data_written = ext4_ordered_flush(fs);

		if (fs->st->wb_len > 0) {
			ext4_wb_flush(fs);
			data_written++;
		}
		if (data_written > 0)
			ext4_dev_sync(fs, "ordered-data");
	}
	unsigned n = fs->st->ckpt.n;
	if (n == 0)
		return; /* nothing dirty */

	unsigned long first = fs->j_first;
	unsigned long maxlen = fs->j_maxlen;
	uint32_t seq = fs->j_sequence;
	unsigned long span = (maxlen > first) ? (maxlen - first) : 0;

	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	int is64 = (be32(jsb->s_feature_incompat) &
		    JBD2_FEATURE_INCOMPAT_64BIT) != 0;
	int csum3 = fs->j_csum3;
	unsigned tagsz = csum3 ? JBD2_TAG3_SIZE : (is64 ? 12u : 8u);

	/* The whole batch must fit one descriptor block + the log.  The batch cap
     * (EXT4_JOURNAL_BATCH) keeps this true; this is a safety fallback. */
	unsigned desc_need =
		sizeof(journal_header_t) + n * tagsz + 16 + (csum3 ? 4u : 0u);
	if (n + 2 > span || desc_need > fs->block_size) {
		WARN_ON_ONCE(1);
		for (unsigned i = 0; i < n; i++)
			ext4_write_block_direct(fs, fs->st->ckpt.blk[i],
						fs->st->ckpt.data[i]);
		ext4_dev_sync(fs, "batch-overflow-direct");
		fs->st->ckpt.n = 0;
		fs->st->epoch_open = 0;
		fs->st->jhead = first;
		return;
	}

	/* 1. mark needs-recovery, durably, once per dirty period (amortised). */
	if (!(fs->sb_copy.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER)) {
		fs->sb_copy.s_feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
		fs->feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
		ext4_write_super(fs);
		ext4_dev_sync(fs, "set-recover");
	}

	/* 2. point the journal sb at the (single) txn start (== j_first). */
	jsb->s_start = __builtin_bswap32((uint32_t)first);
	jsb->s_sequence = __builtin_bswap32(seq);
	ext4_jsb_csum_set(fs->j_sb_buf, csum3);
	jlog_write(fs, 0, fs->j_sb_buf);

	unsigned long pos =
		first; /* descriptor @pos, data @pos+1.., commit @pos+1+n */

	/* 3. descriptor block + data blocks, then the commit block. */
	uint8_t *db = (uint8_t *)ext4_bget(fs);
	uint8_t *cpy = (uint8_t *)ext4_bget(fs);
	if (!db || !cpy) { /* OOM: write the batch direct + sync */
		if (db)
			ext4_bput(fs, db);
		if (cpy)
			ext4_bput(fs, cpy);
		for (unsigned i = 0; i < n; i++)
			ext4_write_block_direct(fs, fs->st->ckpt.blk[i],
						fs->st->ckpt.data[i]);
		ext4_dev_sync(fs, "oom-direct");
		fs->st->ckpt.n = 0;
		fs->st->epoch_open = 0;
		fs->st->jhead = first;
		return;
	}
	mm_memset(db, 0, fs->block_size);
	journal_header_t *dh = (journal_header_t *)db;
	dh->h_magic = __builtin_bswap32(JBD2_MAGIC_NUMBER);
	dh->h_blocktype = __builtin_bswap32(JBD2_DESCRIPTOR_BLOCK);
	dh->h_sequence = __builtin_bswap32(seq);
	unsigned off = sizeof(journal_header_t);
	for (unsigned i = 0; i < n; i++) {
		unsigned long tgt = fs->st->ckpt.blk[i];
		uint32_t flags = 0;
		if (i > 0)
			flags |= JBD2_FLAG_SAME_UUID; /* uuid only after tag0 */
		if (i == n - 1)
			flags |= JBD2_FLAG_LAST_TAG;
		uint32_t fw;
		mm_memcpy(&fw, fs->st->ckpt.data[i], 4);
		int escape = (fw == __builtin_bswap32(JBD2_MAGIC_NUMBER));
		if (escape)
			flags |= JBD2_FLAG_ESCAPE;

		/* The journalled (escaped) copy must exist before the tag csum, which is
         * computed over the exact bytes written to the log. */
		mm_memcpy(cpy, fs->st->ckpt.data[i], fs->block_size);
		if (escape) {
			uint32_t z = 0;
			mm_memcpy(cpy, &z, 4);
		}

		uint8_t *tag = db + off;
		*(uint32_t *)(tag + 0) =
			__builtin_bswap32((uint32_t)(tgt & 0xFFFFFFFFUL));
		if (csum3) { /* 16-byte csum-v3 tag  */
			*(uint32_t *)(tag + JBD2_TAG3_FLAGS_OFF) =
				__builtin_bswap32(flags);
			*(uint32_t *)(tag + JBD2_TAG3_BLKHI_OFF) =
				is64 ? __builtin_bswap32(
					       (uint32_t)(tgt >> 32)) :
				       0;
			uint32_t tc = ext4_jblock_csum(fs->j_csum_seed, seq,
						       cpy, fs->block_size);
			*(uint32_t *)(tag + JBD2_TAG3_CSUM_OFF) =
				__builtin_bswap32(tc);
		} else { /* classic 8/12-byte tag*/
			*(uint16_t *)(tag + 4) = 0; /* t_checksum (no csum) */
			*(uint16_t *)(tag + 6) =
				__builtin_bswap16((uint16_t)flags);
			if (is64)
				*(uint32_t *)(tag + 8) = __builtin_bswap32(
					(uint32_t)(tgt >> 32));
		}
		off += tagsz;
		if (!(flags & JBD2_FLAG_SAME_UUID)) { /* journal uuid follows */
			mm_memcpy(db + off, fs->j_uuid, 16);
			off += 16;
		}
		jlog_write(fs, pos + 1 + i, cpy);
	}
	if (csum3) { /* descriptor tail csum */
		uint32_t *dt = (uint32_t *)(db + fs->block_size - 4);
		*dt = 0;
		*dt = __builtin_bswap32(
			ext4_crc32c(fs->j_csum_seed, db, fs->block_size));
	}
	jlog_write(fs, pos, db); /* the descriptor      */
	mm_memset(cpy, 0, fs->block_size); /* commit block        */
	journal_header_t *ch = (journal_header_t *)cpy;
	ch->h_magic = __builtin_bswap32(JBD2_MAGIC_NUMBER);
	ch->h_blocktype = __builtin_bswap32(JBD2_COMMIT_BLOCK);
	ch->h_sequence = __builtin_bswap32(seq);
	if (csum3) {
		uint32_t *cc = (uint32_t *)(cpy + JBD2_COMMIT_CSUM_OFF);
		*cc = 0;
		*cc = __builtin_bswap32(
			ext4_crc32c(fs->j_csum_seed, cpy, fs->block_size));
	}
	jlog_write(fs, pos + 1 + n, cpy);
	ext4_dev_sync(fs, "commit"); /* commit durable */
	ext4_bput(fs, db);
	ext4_bput(fs, cpy);

	fs->st->jhead = pos + n + 2;
	fs->st->epoch_open = 1;
	fs->j_sequence = seq + 1;

	/* 4. checkpoint: write the batch to its final homes + empty the log. */
	ext4_checkpoint(fs);
}

/* Per-operation commit (outermost ext4_io_unlock): merge this op's captured
 * metadata into the in-memory batch — NO disk I/O — and durably flush the batch
 * only when it fills (or a freed-block forces it).  fsync/sync/unmount flush it
 * explicitly.  Atomicity on a mid-op task death is preserved because st->txn is
 * still per-op: the dead-task handler discards st->txn while the already-merged
 * batch (st->ckpt) of completed ops stays intact. */
static void ext4_txn_flush(ext4_fs_t *fs)
{
	if (!fs->st->txn.active)
		return;
	fs->st->txn.active = 0;
	unsigned n = fs->st->txn.n;
	fs->st->txn.n = 0;
	if (n == 0)
		return; /* read-only op: nothing to commit */

	if (!fs || !fs->j_enabled ||
	    !fs->j_sb_buf) { /* journaling off: direct */
		for (unsigned i = 0; i < n; i++)
			ext4_write_block_direct(fs, fs->st->txn.blk[i],
						fs->st->txn.data[i]);
		return;
	}

	for (unsigned i = 0; i < n;
	     i++) /* accumulate into the batch (in memory) */
		ext4_ckpt_merge(fs, fs->st->txn.blk[i], fs->st->txn.data[i]);
	fs->st->epoch_open = 1;

	/* Flush durably only when the batch is full, or this op freed a block that
     * still has a journal copy (it must be checkpointed before reuse). */
	if (fs->st->ckpt.n >= EXT4_JOURNAL_BATCH)
		ext4_journal_flush(fs);
}

/* Clean the journal: checkpoint any open epoch to its final locations, mark the
 * log empty (s_start=0) and drop needs-recovery.  Called on explicit sync /
 * fsync / shutdown — NOT per commit.  After this the on-disk fs is fully
 * consistent and a reboot finds a clean fs that does not replay. */
static void ext4_journal_clean(ext4_fs_t *fs)
{
	if (!fs || !fs->j_enabled || !fs->j_sb_buf)
		return;
	ext4_journal_flush(
		fs); /* durably commit the batch -> log -> final, s_start=0 */
	if (!(fs->sb_copy.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER))
		return; /* already clean */
	journal_superblock_t *jsb = (journal_superblock_t *)fs->j_sb_buf;
	if (jsb->s_start != 0) { /* ensure log empty on disk   */
		jsb->s_start = 0;
		jsb->s_sequence = __builtin_bswap32(fs->j_sequence);
		ext4_jsb_csum_set(fs->j_sb_buf, fs->j_csum3);
		jlog_write(fs, 0, fs->j_sb_buf);
		ext4_dev_sync(fs, "clean-jsb");
	}
	fs->sb_copy.s_feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
	fs->feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
	ext4_write_super(fs);
	ext4_dev_sync(fs, "clean-recover");
}

static int ext4_state_alloc(ext4_fs_t *fs);

/* Read the superblock and bring the filesystem up in `out' (zeroed by the
 * caller).  `want_ro': the mount will be read-only, so nothing that is only
 * housekeeping is written here; a journal left behind is still replayed. */
static int ext4_mount_sb(const block_device_t *bdev, ext4_fs_t *out,
			 int want_ro)
{
	unsigned ss = bdev->sector_size ? bdev->sector_size : 512;

	/* Find the ext4 filesystem (whole-device or a GPT partition). */
	unsigned long part_lba = ext4_locate_partition(bdev);
	if (part_lba == (unsigned long)-1)
		return ST_NOT_FOUND;

	/* The ext4 superblock is at byte offset 1024 (sector 2 for 512B sectors),
     * relative to the partition start. */
	unsigned long sb_sector = part_lba + EXT4_SUPERBLOCK_OFFSET / ss;
	unsigned sb_sectors = (sizeof(ext4_super_block) + ss - 1) / ss;
	if (sb_sectors < 1)
		sb_sectors = 1;

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
	if (ext4_state_alloc(out) != ST_OK) {
		kfree(raw);
		return ST_NOMEM;
	}
	ext4_fs_t *fs = out;
	mm_memcpy(&out->sb_copy, sb, sizeof(out->sb_copy));
	out->bdev = bdev;
	/* Settle the device's cache-flush support before anything can issue a
	 * barrier (journal replay and the csum-v3 upgrade both do). */
	ext4_probe_dev_sync(out);
	out->part_lba_offset =
		part_lba; /* 0 for whole-device, else GPT part start */
	out->block_size = 1024u << sb->s_log_block_size;
	out->sectors_per_block = out->block_size / ss;
	out->inode_size = (sb->s_rev_level == 0) ? EXT4_GOOD_OLD_INODE_SIZE :
						   sb->s_inode_size;
	if (out->inode_size < 128)
		out->inode_size = 128;
	out->inodes_per_group = sb->s_inodes_per_group;
	out->blocks_per_group = sb->s_blocks_per_group;
	out->inodes_count = sb->s_inodes_count;
	out->blocks_count = (unsigned long)sb->s_blocks_count_lo |
			    ((unsigned long)sb->s_blocks_count_hi << 32);
	out->first_data_block = sb->s_first_data_block;
	out->feature_incompat = sb->s_feature_incompat;
	out->feature_ro_compat = sb->s_feature_ro_compat;
	out->feature_compat = sb->s_feature_compat;
	out->first_ino = (sb->s_rev_level == 0) ? EXT4_GOOD_OLD_FIRST_INO :
						  sb->s_first_ino;
	out->journal_inum = sb->s_journal_inum;
	out->read_only = 0; /* writes enabled */
	/* errors= policy for runtime corruption (ext4_fs_error).  The reference
	 * driver applies a
     * mount-time default of remount-ro on metadata corruption regardless of the
     * on-disk s_errors hint (mke2fs stamps s_errors=continue by default), since
     * leaving a corrupt filesystem writable risks compounding the damage.  We do
     * the same: default to the safe remount-ro, honouring only an explicit panic
     * request from the superblock.  A deliberate errors=continue would require a
     * mount option, which we don't parse yet. */
	out->errors_behavior = (sb->s_errors == EXT4_ERRORS_PANIC) ?
				       EXT4_ERRORS_PANIC :
				       EXT4_ERRORS_RO;

	if (out->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
		out->desc_size = sb->s_desc_size ? sb->s_desc_size : 64;
	else
		out->desc_size = 32;

	if (out->blocks_per_group == 0) {
		kfree(raw);
		return ST_INVALID;
	}
	out->groups_count =
		(unsigned int)((out->blocks_count - out->first_data_block +
				out->blocks_per_group - 1) /
			       out->blocks_per_group);

	/* metadata_csum seed + (verify-only) superblock checksum.  The seed is
     * s_checksum_seed when the CSUM_SEED feature is set, else crc32c(~0, uuid). */
	out->has_metadata_csum = (out->feature_ro_compat &
				  EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
	if (out->feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
		out->csum_seed = sb->s_checksum_seed;
	else
		out->csum_seed = ext4_crc32c(0xFFFFFFFFu, sb->s_uuid,
					     sizeof(sb->s_uuid));
	if (out->has_metadata_csum) {
		uint32_t got = ext4_sb_csum(sb), want = sb->s_checksum;
		if (got != want) {
			/* The superblock is corrupt — we can't trust its geometry for
             * writes.  Degrade to a read-only mount (don't refuse to boot / panic
             * the root fs) and mark it errored in-memory so e2fsck is run. */
			kprintf("ext4: ERROR superblock metadata_csum mismatch "
				"(disk 0x%x computed 0x%x) -> mounting read-only\n",
				want, got);
			out->sb_copy.s_state &= ~(uint16_t)EXT4_VALID_FS;
			out->sb_copy.s_state |= (uint16_t)EXT4_ERROR_FS;
			out->read_only = 1;
		} else
			kprintf("ext4: metadata_csum on; superblock csum verified\n");
	}

	kfree(raw);

	/* Reject incompat features we cannot safely read. */
	uint32_t known_incompat =
		EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_RECOVER |
		EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |
		EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_META_BG |
		EXT4_FEATURE_INCOMPAT_INLINE_DATA |
		EXT4_FEATURE_INCOMPAT_CSUM_SEED;
	if (out->feature_incompat & ~known_incompat) {
		kprintf("ext4: unsupported incompat features 0x%x\n",
			out->feature_incompat & ~known_incompat);
		return ST_UNSUPPORTED;
	}

	ext4_inode_cache_flush(fs); /* reset the parsed-inode cache */
	ext4_mbc_invalidate(fs);
	if (ext4_load_gdt(out) != ST_OK) {
		kprintf("ext4: failed to load group descriptor table\n");
		return ST_IO;
	}

	/* Verify-only: confirm every group descriptor's metadata_csum, which
     * exercises the csum_seed derivation.  Warn with a count; never reject. */
	if (out->has_metadata_csum) {
		unsigned bad = 0;
		for (unsigned int g = 0; g < out->groups_count; g++)
			if (ext4_gd_csum(out, g, &out->gdt[g]) !=
			    out->gdt[g].bg_checksum)
				bad++;
		if (bad) {
			/* A corrupt group descriptor would mis-place bitmaps / inode tables —
             * degrade to read-only and mark errored rather than write through it. */
			kprintf("ext4: ERROR %u/%u group-desc metadata_csum mismatch "
				"-> mounting read-only\n",
				bad, out->groups_count);
			out->sb_copy.s_state &= ~(uint16_t)EXT4_VALID_FS;
			out->sb_copy.s_state |= (uint16_t)EXT4_ERROR_FS;
			out->read_only = 1;
		} else
			kprintf("ext4: all %u group-desc csums verified\n",
				out->groups_count);
	}

	/* Journal replay: apply any committed transactions left in the journal
     * BEFORE any write touches the filesystem.  Triggered by the journal's own
     * log-tail pointer (s_start != 0), NOT just the ext4 RECOVER flag — those
     * are separate sectors and a device write-back cache can persist the
     * journal commit without the RECOVER bit across a crash (seen on VMware),
     * so a RECOVER-only gate silently misses real recovery work. */
	/* The device's own write protection: a replay could not be written,
	 * and neither could clearing RECOVER. */
	int dev_ro = blk_bdev_read_only(bdev);

	if (out->journal_inum != 0 && (out->mnt_opts & EXT4_MNT_NOLOAD)) {
		/* norecovery / noload: the journal is left as it is, and so
		 * is RECOVER -- the next mount without the option replays.
		 * Whatever the journal holds is missing from what is read. */
		if (!want_ro) {
			kprintf("ext4: norecovery needs a read-only mount\n");
			return ST_INVALID;
		}
		int dirty = (out->feature_incompat &
			     EXT4_FEATURE_INCOMPAT_RECOVER) != 0 ||
			    ext4_journal_recover(out, NULL, 1) == ST_ROFS;
		kprintf("ext4: journal not replayed (norecovery)%s\n",
			dirty ? ": it holds changes that are not applied -- the "
				"filesystem may read inconsistent" :
				"");
	} else if (out->journal_inum != 0 && dev_ro &&
		   (out->mnt_opts & EXT4_MNT_WP_STRICT) &&
		   ext4_journal_recover(out, NULL, 1) == ST_ROFS) {
		/* Mounting without the replay would show a filesystem missing
		 * changes it committed; replaying cannot write. */
		kprintf("ext4: the journal needs recovery and the device is "
			"write-protected: not mounted (norecovery mounts it "
			"read-only without replaying, possibly inconsistent)\n");
		return ST_ROFS;
	} else if (out->journal_inum != 0) {
		int replayed = 0;
		int rr = ext4_journal_recover(out, &replayed, 0);
		if (rr == ST_OK) {
			if (replayed) {
				/* Replay rewrote final metadata under us; drop caches that
                 * were populated before/by replay so later reads see the
                 * recovered state. */
				ext4_mbc_invalidate(fs);
				ext4_inode_cache_flush(fs);
				ext4_load_gdt(
					out); /* GDT blocks may have been replayed   */
			}
			/* Clear RECOVER (if it was set) now the journal is
			 * clean again -- on a device that can be written. */
			if ((out->feature_incompat &
			     EXT4_FEATURE_INCOMPAT_RECOVER) &&
			    !(dev_ro && (out->mnt_opts & EXT4_MNT_WP_STRICT))) {
				out->feature_incompat &=
					~EXT4_FEATURE_INCOMPAT_RECOVER;
				out->sb_copy.s_feature_incompat &=
					~EXT4_FEATURE_INCOMPAT_RECOVER;
				ext4_write_super(out);
				ext4_dev_sync(out, "mount-clear-recover");
			}
		} else {
			/* Leave RECOVER set so the next mount retries (replay is
             * idempotent).  Warn loudly: this matches today's behaviour of
             * mounting a dirty journal, no worse, and recoverable on reboot. */
			kprintf("ext4: WARNING journal replay failed (%d); RECOVER left set\n",
				rr);
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
		int is64 = (out->feature_incompat &
			    EXT4_FEATURE_INCOMPAT_64BIT) != 0;
		uint64_t fb = 0;
		uint32_t fi = 0;
		for (unsigned g = 0; g < out->groups_count; g++) {
			uint32_t gb = out->gdt[g].bg_free_blocks_count_lo;
			uint32_t gi = out->gdt[g].bg_free_inodes_count_lo;
			if (is64) {
				gb |= (uint32_t)out->gdt[g]
					      .bg_free_blocks_count_hi
				      << 16;
				gi |= (uint32_t)out->gdt[g]
					      .bg_free_inodes_count_hi
				      << 16;
			}
			fb += gb;
			fi += gi;
		}
		uint32_t fb_lo = (uint32_t)fb, fb_hi = (uint32_t)(fb >> 32);
		if (out->sb_copy.s_free_blocks_count_lo != fb_lo ||
		    out->sb_copy.s_free_blocks_count_hi != fb_hi ||
		    out->sb_copy.s_free_inodes_count != fi) {
			out->sb_copy.s_free_blocks_count_lo = fb_lo;
			out->sb_copy.s_free_blocks_count_hi = fb_hi;
			out->sb_copy.s_free_inodes_count = fi;
			/* A read-only mount uses the right counts without
			 * storing them. */
			if (!want_ro) {
				ext4_write_super(out);
				ext4_dev_sync(out, "mount-resync-counts");
				kprintf("ext4: resynced superblock free counts (blocks=%lu inodes=%u)\n",
					(unsigned long)fb, fi);
			}
		}
	}

	/* Set up journaled writes (ordered mode).  Disabled unless the journal
     * exists, matches our block size, and uses only features we can WRITE
     * correctly.  We stamp jbd2 csum v3, so a csum-v3 journal is supported; csum
     * v2, async-commit and fast-commit remain unsupported on the write side and
     * fall back to direct writes.  On a metadata_csum fs whose journal is still
     * plain (mke2fs ships it that way — the kernel upgrades it on first rw mount)
     * we upgrade it to csum-v3 here, matching the reference driver, so our
     * own images get
     * csum-protected journaling.  Must run after recovery so j_sequence reflects
     * the post-recovery journal state (and the log is clean before any upgrade). */
	out->j_enabled = 0;
	out->j_sb_buf = 0;
	out->j_csum3 = 0;
	/* Not on a journal left unreplayed (norecovery): nothing may ever
	 * be committed to it or mark it clean -- that would throw away what
	 * it holds. */
	if ((out->feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) &&
	    out->journal_inum != 0 && !(out->mnt_opts & EXT4_MNT_NOLOAD)) {
		out->j_sb_buf = (uint8_t *)kalloc(out->block_size);
		unsigned long jpbn =
			out->j_sb_buf ?
				ext4_block_map(out, out->journal_inum, 0) :
				0;
		if (out->j_sb_buf && jpbn &&
		    ext4_read_block(out, jpbn, out->j_sb_buf) == ST_OK) {
			journal_superblock_t *jsb =
				(journal_superblock_t *)out->j_sb_buf;
			uint32_t jmag = be32(jsb->s_header.h_magic);
			uint32_t jbt = be32(jsb->s_header.h_blocktype);
			uint32_t jinc = be32(jsb->s_feature_incompat);
			uint32_t jbad = JBD2_FEATURE_INCOMPAT_CSUM_V2 |
					JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT |
					JBD2_FEATURE_INCOMPAT_FAST_COMMIT;
			if (jmag == JBD2_MAGIC_NUMBER &&
			    (jbt == JBD2_SUPERBLOCK_V1 ||
			     jbt == JBD2_SUPERBLOCK_V2) &&
			    be32(jsb->s_blocksize) == out->block_size &&
			    !(jinc & jbad)) {
				out->j_first = be32(jsb->s_first);
				out->j_maxlen = be32(jsb->s_maxlen);
				out->j_sequence = be32(jsb->s_sequence);
				if (out->j_sequence == 0)
					out->j_sequence = 1; /* jbd2 seq >= 1 */
				mm_memcpy(out->j_uuid, jsb->s_uuid, 16);
				out->j_sb_pbn = jpbn;
				out->j_csum3 =
					(jinc &
					 JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0;
				/* seed for tag/descriptor/commit csums = crc32c(~0, journal uuid) */
				out->j_csum_seed = ext4_crc32c(0xFFFFFFFFu,
							       out->j_uuid, 16);
				if (out->j_maxlen > out->j_first + 3)
					out->j_enabled = 1;
				/* Deferred-checkpoint epoch state.  The log is empty here
                 * (recovery, run earlier, left s_start=0), so the first epoch
                 * starts writing at j_first. */
				fs->st->jhead = out->j_first;
				fs->st->epoch_open = 0;
				fs->st->ckpt.n = 0;
				fs->st->epoch_seq = 0;

				/* Make the journal a well-formed csum-v3 journal on a
                 * metadata_csum fs, as the reference driver does on first rw mount.
                 * Fires when the journal is still plain OR was left half-upgraded
                 * (CSUM_V3 set but s_checksum_type unset — jbd2/e2fsck reject
                 * that).  Only with a CLEAN log (s_start==0, true after recovery /
                 * on a clean image) — flipping journal features mid-log would
                 * corrupt replay — and only on a writable mount.  Sets CSUM_V3 +
                 * s_checksum_type=crc32c, clears the deprecated v1 compat-checksum,
                 * and restamps the journal-sb csum (seed ~0).  On persist failure,
                 * fully restore the in-memory sb so later commits never write a
                 * half-upgraded superblock. */
				int type_ok =
					(out->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF] ==
					 JBD2_CRC32C_CHKSUM);
				if (out->j_enabled && out->has_metadata_csum &&
				    !out->read_only && !want_ro &&
				    be32(jsb->s_start) == 0 &&
				    (!out->j_csum3 || !type_ok)) {
					int was_v3 = out->j_csum3;
					uint32_t o_inc =
						jsb->s_feature_incompat; /* be, for revert  */
					uint32_t o_comp = jsb->s_feature_compat;
					uint8_t o_type =
						out->j_sb_buf
							[JBD2_SB_CSUM_TYPE_OFF];

					jsb->s_feature_incompat = __builtin_bswap32(
						jinc |
						JBD2_FEATURE_INCOMPAT_CSUM_V3);
					jsb->s_feature_compat = __builtin_bswap32(
						be32(jsb->s_feature_compat) &
						~JBD2_FEATURE_COMPAT_CHECKSUM);
					out->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF] =
						JBD2_CRC32C_CHKSUM;
					ext4_jsb_csum_set(out->j_sb_buf, 1);

					if (jlog_write(out, 0, out->j_sb_buf) ==
					    ST_OK) {
						ext4_dev_sync(out,
							      "mount-jsb-csum3");
						out->j_csum3 = 1;
						kprintf("ext4: %s journal to csum-v3 (metadata_csum fs)\n",
							was_v3 ?
								"fixed s_checksum_type on" :
								"upgraded plain");
					} else {
						jsb->s_feature_incompat = o_inc;
						jsb->s_feature_compat = o_comp;
						out->j_sb_buf
							[JBD2_SB_CSUM_TYPE_OFF] =
							o_type;
						ext4_jsb_csum_set(
							out->j_sb_buf,
							was_v3); /* restore prior */
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
			if (out->j_sb_buf) {
				kfree(out->j_sb_buf);
				out->j_sb_buf = 0;
			}
		}
	}
	return ST_OK;
}

/* ---- The per-filesystem state: allocation and release ------------------- */

static int ext4_state_alloc(ext4_fs_t *fs)
{
	struct ext4_state *st = (struct ext4_state *)kalloc(sizeof(*st));

	if (!st)
		return ST_NOMEM;
	mm_memset(st, 0, sizeof(*st));
	ext4_rwsem_init(&st->meta, "ext4_meta");
	for (int i = 0; i < EXT4_ILOCKS; i++)
		ext4_rwsem_init(&st->ilock[i], "ext4_ino");
	for (int i = 0; i < EXT4_BGLOCKS; i++)
		ext4_rwsem_init(&st->bglock[i], "ext4_bg");
	spinlock_init(&st->mbc_lock, "ext4_mbc");
	spinlock_init(&st->bpool_lock, "ext4_bpool");
	spinlock_init(&st->di_lock, "ext4_di");
	spinlock_init(&st->ic_lock, "ext4_ic");
	spinlock_init(&st->ord_lock, "ext4_ordered");
	st->gd_lo = (unsigned)-1;
	st->gd_seen_lo = (unsigned)-1;
	st->cwd_ino = EXT4_ROOT_INO;
	fs->st = st;
	return ST_OK;
}

/* Free everything a filesystem allocated, its state included.  Nothing may
 * use it any more: the caches were told (vfs_sb_release_caches) and the
 * commit thread lets go of it (ext4_commit_detach). */
static void ext4_fs_release(ext4_fs_t *fs)
{
	struct ext4_state *st = fs->st;

	if (st) {
		for (unsigned i = 0; i < st->txn.cap && st->txn.data; i++)
			if (st->txn.data[i])
				kfree(st->txn.data[i]);
		if (st->txn.data)
			kfree(st->txn.data);
		if (st->txn.blk)
			kfree(st->txn.blk);
		for (unsigned i = 0; i < st->ckpt.cap && st->ckpt.data; i++)
			if (st->ckpt.data[i])
				kfree(st->ckpt.data[i]);
		if (st->ckpt.data)
			kfree(st->ckpt.data);
		if (st->ckpt.blk)
			kfree(st->ckpt.blk);
		long cached = 0;
		for (int i = 0; i < EXT4_MBC_ENTRIES; i++)
			if (st->mbc[i].data) {
				cached += st->mbc[i].size;
				kfree(st->mbc[i].data);
			}
		for (int i = 0; i < EXT4_BPOOL; i++)
			if (st->bpool[i].data) {
				WARN_ON(st->bpool[i].busy);
				cached += st->bpool[i].size;
				kfree(st->bpool[i].data);
			}
		mm_buffercache_account(-cached);
		if (st->gd_dirty)
			kfree(st->gd_dirty);
		if (st->wbounce)
			kfree(st->wbounce);
		if (st->gd_buf)
			kfree(st->gd_buf);
		if (st->ord_ino)
			kfree(st->ord_ino);
		kfree(st);
		fs->st = NULL;
	}
	if (fs->gdt) {
		kfree(fs->gdt);
		fs->gdt = NULL;
	}
	if (fs->j_sb_buf) {
		kfree(fs->j_sb_buf);
		fs->j_sb_buf = NULL;
	}
}

static int ext4_mount_opts(const block_device_t *bdev, ext4_fs_t *out,
			   int read_only, unsigned opts)
{
	if (!bdev || !out)
		return ST_INVALID;
	mm_memset(out, 0, sizeof(*out));
	out->mnt_opts = opts;
	int r = ext4_mount_sb(bdev, out, read_only);
	if (r != ST_OK) {
		ext4_fs_release(out);
		return r;
	}
	if (read_only)
		out->read_only = 1;
	return ST_OK;
}

int ext4_mount(const block_device_t *bdev, ext4_fs_t *out, int read_only)
{
	return ext4_mount_opts(bdev, out, read_only, 0);
}

/* Periodic journal commit, mirroring a real journal's ~5s commit interval: the
 * in-memory batch is committed durably on this timer too, not only when it fills
 * or on fsync — so a crash loses at most this interval's worth of recent work
 * instead of an unbounded amount.  Runs in its own kernel task because the flush
 * takes the I/O mutex and sleeps on disk I/O (so it can't live in a timer IRQ).
 *
 * One task serves every mounted ext4 filesystem: they are on a list, and a
 * pass marks the filesystem it is working on (commit_busy) so an unmount can
 * take it off the list and wait for the pass to finish before freeing it. */
#define EXT4_COMMIT_INTERVAL_SEC 5
static uint8_t s_flush_stack[16384] __attribute__((aligned(16)));
static int s_flush_thread_started;
static ext4_fs_t *s_commit_list;
static spinlock_t s_commit_lock = SPINLOCK_INIT("ext4_commit");

/* One commit pass over `fs'. */
static void ext4_commit_pass(ext4_fs_t *fs)
{
	/* di_n is read without its lock on purpose: it is only being
	 * asked "is there anything to do", and a stale answer costs one
	 * interval, not correctness.  It is here so that a filesystem
	 * with no journal -- which has no batch to commit and would
	 * otherwise skip this whole block -- still stores the sizes
	 * write() left in memory on a timer. */
	if (fs->read_only || !(fs->j_enabled || fs->st->di_n))
		return;
	if (fs->j_enabled)
		ext4_ordered_writeback(fs);
	ext4_io_lock(fs);
	/* Sizes and timestamps that write() left in memory get
	 * stored here, on the same schedule and for the same
	 * reason as the pages: this thread holds no filesystem
	 * lock coming in, so it can take the metadata lock
	 * exclusively, and a program is not made to wait for
	 * it.  The inode blocks it writes join this thread's
	 * own transaction and are committed by the release of
	 * the lock below, so they reach the log on the next
	 * pass rather than this one -- which is what a
	 * five-second commit interval means in any case. */
	ext4_di_flush(fs);
	if (fs->j_enabled)
		ext4_journal_flush(
			fs); /* commit + checkpoint the batch (no-op if empty) */
	ext4_io_unlock(fs);
}

static void ext4_flush_thread(void *arg)
{
	(void)arg;
	for (;;) {
		task_t *self = sched_current();
		if (self) { /* sleep ~EXT4_COMMIT_INTERVAL_SEC */
			self->wait_channel = (void *)&s_commit_list;
			self->wakeup_tick = timer_ticks() +
					    (uint64_t)EXT4_COMMIT_INTERVAL_SEC *
						    timer_get_frequency();
			task_set_blocked(self);
			sched_schedule();
			self->wakeup_tick = 0;
			self->wait_channel = NULL;
			if (self->state != TASK_RUNNING)
				self->state = TASK_RUNNING;
		}
		/* Store whatever the cache is holding, if the timer has asked
		 * for it.  This thread is the place for it: it holds no
		 * filesystem lock, so it may take the I/O lock for writing.
		 * Doing the same from a cache lookup would mean asking for that
		 * lock while a reader already holds the metadata lock, which
		 * does not end. */
		pagecache_writeback_if_due();

		/* Each filesystem in turn, marked busy while its pass runs. */
		ext4_fs_t *done = NULL;
		for (;;) {
			uint64_t fl;
			ext4_fs_t *fs;

			spin_lock_irqsave(&s_commit_lock, &fl);
			fs = s_commit_list;
			if (done) {
				/* The one after the last done, if it is still
				 * on the list; else the walk is over. */
				ext4_fs_t *p = s_commit_list;
				while (p && p != done)
					p = p->st->commit_next;
				fs = p ? p->st->commit_next : NULL;
				done->st->commit_busy--;
			}
			if (fs)
				fs->st->commit_busy++;
			spin_unlock_irqrestore(&s_commit_lock, fl);
			if (!fs)
				break;
			ext4_commit_pass(fs);
			done = fs;
		}
	}
}

/* Put a filesystem on the commit thread's list (starting the thread with the
 * first journalled one), or take it off -- waiting for a pass that is
 * working on it to finish. */
static void ext4_commit_attach(ext4_fs_t *fs)
{
	uint64_t fl;

	spin_lock_irqsave(&s_commit_lock, &fl);
	fs->st->commit_next = s_commit_list;
	s_commit_list = fs;
	spin_unlock_irqrestore(&s_commit_lock, fl);

	/* Start the periodic commit task once a journalled filesystem is live.
	 * It is a TASK_KERNEL thread, so sched_add_task makes it unkillable by
	 * design (sys_kill and sched_signal_pgrp both reject TASK_KERNEL with
	 * -EPERM).  Name it for ps, which shows kernel threads bracketed, e.g.
	 * "[ext4-commit]". */
	if (!s_flush_thread_started && fs->j_enabled) {
		s_flush_thread_started = 1;
		task_t *t = sched_add_task(ext4_flush_thread, 0, s_flush_stack,
					   sizeof(s_flush_stack));
		if (t) {
			const char *nm = "ext4-commit";
			unsigned i = 0;
			for (; nm[i] && i < sizeof(t->comm) - 1; i++)
				t->comm[i] = nm[i];
			t->comm[i] = '\0';
		}
	}
}

static void ext4_commit_detach(ext4_fs_t *fs)
{
	uint64_t fl;

	spin_lock_irqsave(&s_commit_lock, &fl);
	ext4_fs_t **pp = &s_commit_list;
	while (*pp && *pp != fs)
		pp = &(*pp)->st->commit_next;
	if (*pp)
		*pp = fs->st->commit_next;
	fs->st->commit_next = NULL;
	spin_unlock_irqrestore(&s_commit_lock, fl);
	while (__atomic_load_n(&fs->st->commit_busy, __ATOMIC_ACQUIRE) != 0)
		sched_yield_in_kernel();
}

/* ---- Groups left flagged UNINIT with allocations in them ---------------
 *
 * Earlier LikeOS drivers allocated blocks and inodes inside BLOCK_UNINIT /
 * INODE_UNINIT groups -- writing real bitmaps (with their checksums) and
 * lowering the descriptor counts -- but never cleared the flags.  A driver
 * that believes the flags computes "all free" for such a group and hands out
 * blocks and inodes that live files own.
 *
 * So a flag is believed only while the descriptor still shows the state the
 * flag describes: INODE_UNINIT with every inode free and no directory,
 * BLOCK_UNINIT with exactly the free count of the computed bitmap (what mkfs
 * and resize2fs store).  Any other group is made initialized from the bitmap
 * on disk: flag cleared, bitmap written back with its checksum, free count
 * from the bitmap, and bg_itable_unused moved below the highest inode in use
 * (e2fsck's rule).  Run once per rw mount, before the first allocation, in
 * journal transactions of a bounded number of groups.
 *
 * Those drivers started from the block on disk of a flagged group -- never
 * written by mkfs, so zeros or old contents -- which knows nothing of the
 * group's own metadata (superblock and descriptor backups, reserved
 * descriptor blocks, bitmaps and inode table when they live in the group).
 * Allocating first fit, they handed those blocks to files.  The repair keeps
 * every such block marked in use and leaves file data alone; the groups are
 * named on the console and the file system is marked as having errors, so
 * the next e2fsck runs a full check (which sorts out the shared blocks). */

/* Groups found with their metadata blocks handed out (named once). */
#define EXT4_LEGACY_NAMED 48
struct ext4_legacy_damage {
	unsigned n; /* groups found */
	unsigned g[EXT4_LEGACY_NAMED]; /* the first of them */
	unsigned long meta_used; /* metadata blocks in use by files */
	/* Of those, blocks this driver itself writes: the group's own
	 * bitmaps and inode table, a primary descriptor block.  A file
	 * sharing one corrupts it, or is corrupted by it, on the next
	 * write -- unlike backups, which nothing here writes. */
	unsigned long live;
};

/* 1 when the flagged bitmap of group `g' was found in use and rewritten,
 * 0 when the flag is believable, < 0 on an I/O error or a corrupt bitmap
 * (recorded through ext4_fs_error). */
static int ext4_fix_inode_uninit(ext4_fs_t *fs, unsigned g, uint8_t *bm)
{
	unsigned long ipg = fs->inodes_per_group;
	unsigned long dfree = fs->gdt[g].bg_free_inodes_count_lo;

	if (!(fs->gdt[g].bg_flags & EXT4_BG_INODE_UNINIT))
		return 0;
	if (dfree == ipg && fs->gdt[g].bg_used_dirs_count_lo == 0)
		return 0;
	int st = ext4_read_block(fs, ext4_gd_inode_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st == EXT4_BM_CORRUPT ? ST_IO : st;
	/* The flag is what is in doubt: compare the checksum itself (the
	 * _ok form takes any flagged bitmap as good). */
	if (!ext4_inode_bitmap_csum_match(fs, g, bm)) {
		/* Inodes are in use and there is no bitmap to say which:
		 * handing any out could reuse a live one. */
		ext4_fs_error(fs,
			      "inode bitmap of an INODE_UNINIT group in use fails its checksum",
			      0);
		return EXT4_BM_CORRUPT;
	}
	ext4_mark_bitmap_end((unsigned)ipg, fs->block_size * 8, bm);
	unsigned long nfree = 0, hi = 0;
	for (unsigned long i = 0; i < ipg; i++) {
		if (ext4_bm_test(bm, (unsigned)i))
			hi = i + 1;
		else
			nfree++;
	}
	kprintf("ext4: group %u: INODE_UNINIT set with %lu inode(s) in use; taking the bitmap on disk\n",
		g, ipg - nfree);
	ext4_inode_bitmap_csum_set(fs, g, bm);
	st = ext4_write_block(fs, ext4_gd_inode_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st;
	fs->gdt[g].bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
	if (nfree != dfree) {
		fs->gdt[g].bg_free_inodes_count_lo = (uint16_t)nfree;
		fs->sb_copy.s_free_inodes_count =
			(uint32_t)((long)fs->sb_copy.s_free_inodes_count +
				   (long)nfree - (long)dfree);
	}
	if (ext4_itable_unused(fs, g) > ipg - hi)
		ext4_itable_unused_set(fs, g, (unsigned)(ipg - hi));
	ext4_gd_dirty(fs, g);
	return 1;
}

/* The bitmap on disk of flagged group `g' is being taken: count the group's
 * own metadata blocks (the computed bitmap `calc' without its padding) that
 * it shows in use.  An old driver's bitmap started without them, so one shown
 * in use was given to a file: file data and metadata share that block.  Ones
 * shown free were never handed out (or only to a file deleted since; e2fsck
 * finds nothing there) and are merely counted. */
static int ext4_legacy_bit_in_use(ext4_fs_t *fs, unsigned g,
				  const uint8_t *bm, unsigned long b)
{
	return ext4_block_in_group(fs, b, g) &&
	       ext4_bm_test(bm, b - ext4_group_first_block(fs, g));
}

static void ext4_legacy_meta_check(ext4_fs_t *fs, unsigned g,
				   const uint8_t *bm, const uint8_t *calc,
				   struct ext4_legacy_damage *dmg)
{
	unsigned n = ext4_blocks_in_group(fs, g);
	unsigned long used = 0, live = 0;

	for (unsigned b = 0; b < n; b++) {
		if (!ext4_bm_test(calc, b))
			continue;
		if (ext4_bm_test(bm, b))
			used++;
	}
	if (!used)
		return; /* no metadata block given to a file */

	/* Of those, the blocks this driver itself writes. */
	if (ext4_legacy_bit_in_use(fs, g, bm, ext4_gd_block_bitmap(fs, g)))
		live++;
	if (ext4_legacy_bit_in_use(fs, g, bm, ext4_gd_inode_bitmap(fs, g)))
		live++;
	unsigned long itbl = ext4_inode_table_block(fs, &fs->gdt[g]);
	unsigned long itb = ext4_itable_blocks(fs);
	for (unsigned long t = itbl; t < itbl + itb; t++)
		if (ext4_legacy_bit_in_use(fs, g, bm, t))
			live++;
	/* The primary descriptor table, right behind the superblock. */
	unsigned ndesc = ext4_gdt_blocks(fs);
	for (unsigned nr = 0; nr < ndesc; nr++)
		if (ext4_legacy_bit_in_use(fs, g, bm,
					   fs->first_data_block + 1ul + nr))
			live++;

	if (dmg->n < EXT4_LEGACY_NAMED)
		dmg->g[dmg->n] = g;
	dmg->n++;
	dmg->meta_used += used;
	dmg->live += live;
}

static int ext4_fix_block_uninit(ext4_fs_t *fs, unsigned g, uint8_t *bm,
				 uint8_t *calc, struct ext4_legacy_damage *dmg)
{
	if (!(fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT))
		return 0;
	int st = ext4_init_block_bitmap(fs, g, calc);
	if (st != ST_OK)
		return st;
	unsigned long dfree = fs->gdt[g].bg_free_blocks_count_lo;
	unsigned long cfree = ext4_bitmap_free_blocks(fs, g, calc);
	if (dfree == cfree)
		return 0;
	st = ext4_read_block(fs, ext4_gd_block_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st == EXT4_BM_CORRUPT ? ST_IO : st;
	/* The flag is what is in doubt: compare the checksum itself (the
	 * _ok form takes any flagged bitmap as good). */
	if (!ext4_block_bitmap_csum_match(fs, g, bm)) {
		/* Not a bitmap any driver wrote: every writer stamps the
		 * checksum.  So nothing was allocated here through it, and
		 * the count disagrees with the computation only. */
		if (dfree < cfree) {
			/* The descriptor counts blocks in use that the
			 * computed bitmap would hand out, and nothing on
			 * disk says which: the safe answer is none. */
			kprintf("ext4: group %u: BLOCK_UNINIT free count %lu below the computed %lu, bitmap on disk fails its checksum\n",
				g, dfree, cfree);
			ext4_fs_error(fs,
				      "block bitmap of a BLOCK_UNINIT group in use fails its checksum",
				      0);
			return EXT4_BM_CORRUPT;
		}
		/* More free than computed: the computed bitmap marks every
		 * block the descriptor counts in use and more; taking it
		 * frees nothing. */
		kprintf("ext4: group %u: BLOCK_UNINIT free count %lu, computed %lu, no valid bitmap on disk; using the computed one\n",
			g, dfree, cfree);
		mm_memcpy(bm, calc, fs->block_size);
	} else {
		ext4_legacy_meta_check(fs, g, bm, calc, dmg);
		/* The group's own metadata and the padding stay in use
		 * whatever the bitmap on disk says. */
		for (unsigned long i = 0; i < fs->block_size; i++)
			bm[i] |= calc[i];
		kprintf("ext4: group %u: BLOCK_UNINIT set with blocks in use (free %lu, computed %lu); taking the bitmap on disk\n",
			g, dfree, cfree);
	}
	unsigned long nfree = ext4_bitmap_free_blocks(fs, g, bm);
	ext4_block_bitmap_csum_set(fs, g, bm);
	st = ext4_write_block(fs, ext4_gd_block_bitmap(fs, g), bm);
	if (st != ST_OK)
		return st;
	fs->gdt[g].bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
	fs->gdt[g].bg_free_blocks_count_lo = (uint16_t)nfree;
	fs->sb_copy.s_free_blocks_count_lo += (uint32_t)(nfree - dfree);
	ext4_gd_dirty(fs, g);
	return 1;
}

/* Name the groups whose metadata an old driver gave out, once, loudly, and
 * mark the file system as having errors so e2fsck checks it in full.  File
 * data is not touched: which file owns a shared block, and which copy is
 * right, is e2fsck's business (it clones multiply-claimed blocks). */
static void ext4_legacy_damage_report(ext4_fs_t *fs,
				      const struct ext4_legacy_damage *dmg)
{
	char line[160];
	unsigned k = 0, i;

	if (!dmg->n)
		return;
	kprintf("ext4: ****************************************************************\n");
	kprintf("ext4: WARNING: an earlier LikeOS ext4 driver allocated blocks reserved\n");
	kprintf("ext4: for file system metadata (superblock / group descriptor backups,\n");
	kprintf("ext4: reserved descriptor blocks, bitmaps, inode tables) to files in\n");
	kprintf("ext4: %u group(s):\n", dmg->n);
	for (i = 0; i < dmg->n && i < EXT4_LEGACY_NAMED; i++) {
		int w = ksnprintf(line + k, sizeof(line) - k, "%s%u",
				  k ? " " : "", dmg->g[i]);
		if (w < 0 || (unsigned)w >= sizeof(line) - k)
			break;
		k += (unsigned)w;
		if (k > 64) {
			kprintf("ext4:   %s\n", line);
			k = 0;
		}
	}
	if (k)
		kprintf("ext4:   %s\n", line);
	if (dmg->n > EXT4_LEGACY_NAMED)
		kprintf("ext4:   ... and %u more\n", dmg->n - EXT4_LEGACY_NAMED);
	kprintf("ext4: %lu metadata blocks are shared with file data (%lu of them\n",
		dmg->meta_used, dmg->live);
	kprintf("ext4: bitmaps, inode tables or descriptors).  They all stay\n");
	kprintf("ext4: marked in use; no file data was changed.  Run \"e2fsck -f\"\n");
	kprintf("ext4: on this file system (unmounted) before any tune2fs or resize2fs.\n");
	kprintf("ext4: ****************************************************************\n");
	if (dmg->live) {
		/* Blocks this driver writes (bitmaps, inode tables, primary
		 * descriptors) may be shared with file data: every further
		 * write could corrupt one or the other.  A file system error,
		 * with the errors= reaction (read-only). */
		ext4_fs_error(fs,
			      "file data may share blocks with bitmaps, inode tables or group descriptors",
			      0);
		return;
	}
	/* Only backups and reserved descriptor blocks, which nothing here
	 * writes: recorded without the errors= reaction -- the blocks are
	 * protected now and the system stays usable until e2fsck has run. */
	ext4_record_error(fs);
}

static void ext4_fix_legacy_uninit(ext4_fs_t *fs)
{
	if (fs->read_only || !ext4_uninit_honoured(fs) || fs->groups_count < 2)
		return;
	uint8_t *bm = (uint8_t *)kalloc(fs->block_size);
	uint8_t *calc = (uint8_t *)kalloc(fs->block_size);
	struct ext4_legacy_damage *dmg =
		(struct ext4_legacy_damage *)kalloc(sizeof(*dmg));
	unsigned fixed = 0, in_txn = 0;

	if (!bm || !calc || !dmg) {
		if (bm)
			kfree(bm);
		if (calc)
			kfree(calc);
		if (dmg)
			kfree(dmg);
		/* Cannot check: refuse to allocate on a guess.  Out of memory
		 * says nothing about the disk -- nothing is recorded there;
		 * the latch is set directly, whatever errors= says. */
		kprintf("ext4: ERROR: no memory to check the UNINIT groups; mounting read-only\n");
		fs->read_only = 1;
		return;
	}
	mm_memset(dmg, 0, sizeof(*dmg));
	ext4_io_lock(fs);
	for (unsigned g = 1; g < fs->groups_count && !fs->read_only; g++) {
		int a = ext4_fix_inode_uninit(fs, g, bm);
		int b = a < 0 ? a : ext4_fix_block_uninit(fs, g, bm, calc, dmg);

		if (a < 0 || b < 0) {
			/* A group that could not be checked (I/O) or is
			 * corrupt: allocating anywhere could hit its blocks.
			 * Recorded, and read-only whatever errors= says --
			 * the latch is set here, not left to the policy. */
			if (a != EXT4_BM_CORRUPT && b != EXT4_BM_CORRUPT)
				ext4_fs_error(fs, "cannot check an UNINIT group",
					      0);
			fs->read_only = 1;
			break;
		}
		if (a > 0 || b > 0) {
			fixed++;
			/* Bounded transactions: each commit is consistent on
			 * its own (bitmap, flag and counts of a group go
			 * together). */
			if (++in_txn >= 16) {
				ext4_flush_meta(fs);
				if (fs->st->txn.active && fs->st->meta.wdepth == 1) {
					ext4_txn_flush(fs);
					ext4_txn_begin(fs);
				}
				in_txn = 0;
			}
		}
	}
	if (fixed) {
		fs->meta_dirty = 1;
		ext4_flush_meta(fs);
		kprintf("ext4: %u group(s) with stale UNINIT flags made initialized\n",
			fixed);
	}
	ext4_legacy_damage_report(fs, dmg);
	ext4_io_unlock(fs);
	kfree(bm);
	kfree(calc);
	kfree(dmg);
}

/* Ready a freshly mounted filesystem for use. */
static void ext4_instance_ready(ext4_fs_t *fs)
{
	/* Allocate the persistent write-back/coalescing buffer once (target 1 MiB),
     * while memory is unfragmented so the large contiguous allocation succeeds.
     * Fall back to progressively smaller sizes if a full 1 MiB isn't available —
     * a smaller buffer just coalesces less (still correct). */
	if (!fs->st->wbounce) {
		unsigned ss =
			fs->bdev->sector_size ? fs->bdev->sector_size : 512;
		unsigned long want =
			(unsigned long)EXT4_MAX_SECTORS_PER_READ * ss;
		if (want < fs->block_size)
			want = fs->block_size;
		while (want >= fs->block_size) {
			fs->st->wbounce = (uint8_t *)kalloc(want);
			if (fs->st->wbounce)
				break;
			want /= 2;
		}
		fs->st->wbounce_bytes = fs->st->wbounce ? want : 0;
	}
	/* Persistent group-descriptor scratch block, so a write never silently drops
     * a GD update on a transient kalloc failure (would corrupt free counts/csums). */
	if (!fs->st->gd_buf) {
		fs->st->gd_buf = (uint8_t *)kalloc(fs->block_size);
	}
	/* Before anything can allocate: groups an earlier driver used while
	 * leaving them flagged uninitialized. */
	ext4_fix_legacy_uninit(fs);
	/* The periodic commit (started with the first journalled filesystem). */
	ext4_commit_attach(fs);
}

int ext4_vfs_register_root(ext4_fs_t *fs)
{
	if (!fs || !fs->st)
		return ST_INVALID;
	fs->st->is_root = 1;
	fs->st->cwd_ino = EXT4_ROOT_INO;
	ext4_sb_attach(fs);
	/* The caches' superblock for whole-system flushes (pagecache_sync). */
	g_root_sb = &fs->sb;
	int r = vfs_register_root(&ext4_vfs_ops, fs, &fs->sb, &ext4_fstype,
				  "ext4");
	if (r != ST_OK)
		return r;
	fs->mnt = vfs_mnt_root();
	ext4_instance_ready(fs);
	return ST_OK;
}

/* ---- mount(2) ------------------------------------------------------------ */

/* Is there an ext4 (ext2/ext3) superblock on `bdev'?  For fstype "auto". */
static int ext4_fst_probe(struct block_device *bdev)
{
	const block_device_t *b = bdev;
	unsigned ss = b->sector_size ? b->sector_size : 512;
	unsigned n = (EXT4_SUPERBLOCK_OFFSET + 1024 + ss - 1) / ss;
	uint8_t *buf = (uint8_t *)kalloc((unsigned long)n * ss);
	int ok = 0;

	if (!buf)
		return 0;
	if (ext4_read_sectors(b, 0, n, buf) == ST_OK) {
		const ext4_super_block *sb =
			(const ext4_super_block *)(buf +
						   EXT4_SUPERBLOCK_OFFSET);
		ok = (sb->s_magic == EXT4_SUPER_MAGIC);
	}
	kfree(buf);
	return ok;
}

/* ext4's own options in a mount(2) option string: "norecovery" (or
 * "noload") skips the journal replay.  The generic ones are the VFS's, and
 * the rest are accepted and change nothing. */
static unsigned ext4_fst_opts(const char *data)
{
	const char *cur = data, *key, *val;
	unsigned opts = EXT4_MNT_WP_STRICT;
	char tmp[64];

	while (data && vfs_next_option(&cur, tmp, sizeof(tmp), &key, &val))
		if (!kstrcmp(key, "norecovery") || !kstrcmp(key, "noload"))
			opts |= EXT4_MNT_NOLOAD;
	return opts;
}

static int ext4_fst_mount(vfs_mount_t *m, struct block_device *bdev,
			  unsigned long flags, const char *data)
{
	if (!bdev)
		return ST_INVALID;
	ext4_fs_t *fs = (ext4_fs_t *)kalloc(sizeof(*fs));
	if (!fs)
		return ST_NOMEM;
	int r = ext4_mount_opts(bdev, fs, (flags & VFS_MS_RDONLY) != 0,
				ext4_fst_opts(data));
	if (r != ST_OK) {
		kfree(fs);
		return r;
	}
	ext4_sb_attach(fs);
	fs->mnt = m;
	m->ops = &ext4_vfs_ops;
	m->fs_data = fs;
	m->sb = &fs->sb;
	ext4_instance_ready(fs);
	return ST_OK;
}

/* The end of an unmount: nothing is open and every page and inode was
 * stored.  Commit what is left and leave the journal clean, as sync() does,
 * then let go of every cached piece of the filesystem and free it. */
static int ext4_fst_unmount(vfs_mount_t *m)
{
	ext4_fs_t *fs = ext4_fs_of(m);

	if (!fs || !fs->st)
		return ST_INVALID;
	ext4_commit_detach(fs);
	if (!fs->read_only)
		ext4_sync_fs(fs);
	if (fs->st->wb_err)
		kprintf("ext4: data write errors on %s before unmount\n",
			m->path);
	vfs_sb_release_caches(&fs->sb);
	ext4_fs_release(fs);
	m->fs_data = NULL;
	m->sb = NULL;
	kfree(fs);
	return ST_OK;
}

static int ext4_fst_remount(vfs_mount_t *m, unsigned long flags,
			    const char *data)
{
	ext4_fs_t *fs = ext4_fs_of(m);
	int want_ro = (flags & VFS_MS_RDONLY) != 0;

	(void)data;
	if (!fs || !fs->st)
		return ST_INVALID;
	if (want_ro && !fs->read_only) {
		/* A file open for writing would be left holding a promise
		 * the filesystem can no longer keep. */
		if (__atomic_load_n(&m->open_writers, __ATOMIC_SEQ_CST) > 0)
			return ST_BUSY;
		ext4_sync_fs(fs);
		ext4_io_lock(fs);
		fs->read_only = 1;
		ext4_io_unlock(fs);
	} else if (!want_ro && fs->read_only) {
		/* Read-only because of an error found on it: a remount does
		 * not make it trustworthy again; fsck does. */
		if (fs->sb_copy.s_state & EXT4_ERROR_FS)
			return ST_ROFS;
		/* Mounted without replaying its journal: writing to it would
		 * build on a state the journal says is out of date.  Unmount
		 * and mount it without norecovery. */
		if (fs->mnt_opts & EXT4_MNT_NOLOAD) {
			kprintf("ext4: %s was mounted norecovery: it stays read-only\n",
				m->path);
			return ST_ROFS;
		}
		ext4_io_lock(fs);
		fs->read_only = 0;
		ext4_io_unlock(fs);
		ext4_fix_legacy_uninit(fs);
	}
	return ST_OK;
}

static const char *const ext4_fst_aliases[] = { "ext3", "ext2", NULL };

const vfs_fstype_t ext4_fstype = {
	.name = "ext4",
	.aliases = ext4_fst_aliases,
	.needs_device = 1,
	.mount = ext4_fst_mount,
	.unmount = ext4_fst_unmount,
	.remount = ext4_fst_remount,
	.show_options = NULL,
	.probe = ext4_fst_probe,
};
