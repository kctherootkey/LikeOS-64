// LikeOS - ext4 filesystem driver: metadata_csum (crc32c) and group descriptor
// (crc16) checksums: computing, stamping and verifying them
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

/* ===================================================================
 * metadata_csum (crc32c, Castagnoli) — checksum machinery.
 *
 * The read-side VERIFICATION path computes the checksums the way the
 * on-disk format defines them and WARN (once) if an on-disk value disagrees, so the crc32c
 * implementation + seed derivation can be proven against a real image before
 * any write-side code relies on them.  Nothing here rejects or mutates.
 * =================================================================== */

/* crc32c (reflected polynomial 0x82F63B78).  No implicit pre/post inversion —
 * the caller supplies the seed explicitly (as the on-disk format requires).
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

uint32_t ext4_crc32c(uint32_t crc, const void *buf, unsigned long len)
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
uint32_t ext4_sb_csum(const ext4_super_block *sb)
{
	return ext4_crc32c(0xFFFFFFFFu, sb,
			   __builtin_offsetof(ext4_super_block, s_checksum));
}

/* CRC-16 (polynomial 0x8005, bit-reflected: 0xA001), no implicit
 * inversion -- the checksum of group descriptors without metadata_csum. */
static uint16_t ext4_crc16(uint16_t crc, const void *p, unsigned long len)
{
	const uint8_t *b = (const uint8_t *)p;

	while (len--) {
		crc ^= *b++;
		for (int k = 0; k < 8; k++)
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) :
					  (uint16_t)(crc >> 1);
	}
	return crc;
}

/* Group-descriptor checksum.  metadata_csum form: crc32c seeded with the
 * filesystem's seed over the LE group number, then the descriptor with the
 * 2-byte bg_checksum field skipped, truncated to 16 bits.  The older
 * gdt_csum (uninit_bg) form: crc16 from ~0 over the filesystem UUID, the LE
 * group number and the descriptor the same way (its tail past bg_checksum
 * only with 64-bit descriptors). */
uint16_t ext4_gd_csum(const ext4_fs_t *fs, uint32_t group,
			     const ext4_group_desc *gd)
{
	const unsigned off = __builtin_offsetof(ext4_group_desc, bg_checksum);
	const uint16_t zero = 0;
	uint32_t le_group = group; /* x86 is little-endian */
	if (!fs->has_metadata_csum) {
		uint16_t c = ext4_crc16(0xFFFF, fs->sb_copy.s_uuid,
					sizeof(fs->sb_copy.s_uuid));
		c = ext4_crc16(c, &le_group, sizeof(le_group));
		c = ext4_crc16(c, gd, off);
		if ((fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) &&
		    fs->desc_size > off + 2)
			c = ext4_crc16(c, (const uint8_t *)gd + off + 2,
				       fs->desc_size - (off + 2));
		return c;
	}
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
uint32_t ext4_inode_csum(const ext4_fs_t *fs, unsigned long ino,
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
uint32_t ext4_inode_csum_seed(const ext4_fs_t *fs, unsigned long ino,
				     uint32_t gen)
{
	uint32_t le_ino = (uint32_t)ino;
	uint32_t crc = ext4_crc32c(fs->csum_seed, &le_ino, sizeof(le_ino));
	return ext4_crc32c(crc, &gen, sizeof(gen));
}

/* Stamp the inode checksum into the full on-disk inode bytes `raw`
 * (fs->inode_size long) for inode `ino`.  Mirrors the field-presence rules of
 * ext4_inode_csum (the hi half only when the inode is large enough to hold it). */
void ext4_inode_csum_set(const ext4_fs_t *fs, unsigned long ino,
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
void ext4_dir_csum_set(const ext4_fs_t *fs, unsigned long dir_ino,
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
 * (inodes_per_group+7)/8 bytes, as the on-disk format defines it. */
void ext4_block_bitmap_csum_set(ext4_fs_t *fs, unsigned g,
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
void ext4_inode_bitmap_csum_set(ext4_fs_t *fs, unsigned g,
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

/* Verify a directory leaf's metadata_csum tail.  Returns 1 if OK, or if there is
 * nothing to verify: an fs without metadata_csum, or a block that does NOT end
 * with a dirent-checksum tail (the fake entry inode=0/rec_len=12/ft=0xDE).  The
 * latter guard means an htree dx node (different tail layout), which our linear
 * reader may encounter, is left to e2fsck instead of false-positiving. */
int ext4_dir_csum_ok(const ext4_fs_t *fs, unsigned long dir_ino,
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
int ext4_block_bitmap_csum_match(const ext4_fs_t *fs, unsigned g,
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
int ext4_inode_bitmap_csum_match(const ext4_fs_t *fs, unsigned g,
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
int ext4_block_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g,
				     const uint8_t *bm)
{
	if (fs->gdt[g].bg_flags & EXT4_BG_BLOCK_UNINIT)
		return 1;
	return ext4_block_bitmap_csum_match(fs, g, bm);
}
int ext4_inode_bitmap_csum_ok(const ext4_fs_t *fs, unsigned g,
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
int ext4_extent_block_csum_ok(const ext4_fs_t *fs, unsigned long ino,
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
int ext4_memcmp(const void *a, const void *b, unsigned long n)
{
	const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
	for (unsigned long i = 0; i < n; i++)
		if (pa[i] != pb[i])
			return (int)pa[i] - (int)pb[i];
	return 0;
}
