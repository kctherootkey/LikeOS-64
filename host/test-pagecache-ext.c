/* Host test of the page-cache extensions the NTFS / exFAT / ISO9660 drivers
 * build on (kernel/fs/pagecache.c, vfs_sb.h):
 *
 *   1. pagecache_install() / pagecache_lookup_held(): fill, zero tail, hold
 *      counts, the cached copy winning a race, invalidation of a held page;
 *   2. the coalesced read with blocks smaller than a page on a filesystem
 *      with positional block ids: scrambled, partly-hole and all-hole pages
 *      read correctly (the per-block fallback), contiguous runs still go to
 *      the device as one command, holes in blocks larger than a page;
 *   3. delayed allocation at writeback (vfs_sb_ops_t.alloc_blocks): one call
 *      per run of unplaced pages, placed data written where the hook put it,
 *      LBA 0 never written, a failing hook keeps the pages dirty and nothing
 *      is written, VFS_SB_ALLOC_NONE keeps the old behaviour, a filesystem
 *      without the hook behaves as before; for one-block pages (the batch
 *      path), blocks smaller than a page (the per-page paths of
 *      flush_file / flush_all) and the reclaim path;
 *   4. write errors on the per-page paths (blocks smaller or larger than a
 *      page): the page stays dirty and uncounted, and is stored once the
 *      device works; a page invalidated while writeback waited for the I/O
 *      lock is never written (flush_file, flush_all, reclaim).
 *
 * The filesystem under test is a fake one over a RAM disk of 512-byte
 * sectors: streams with a block -> LBA table (0 = hole / not yet placed) and
 * a "reserved" mark per block for delayed allocation.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/fs/pagecache.h>
#include <kernel/fs/vfs_sb.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/status.h>

extern int printf(const char *, ...);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern int memcmp(const void *, const void *, unsigned long);
extern void exit(int);

extern long test_allocs, test_pages;
extern int test_warnings, test_locks_held;
extern uint64_t test_irq_flag;

static int failures;
#define CHECK(cond)                                                      \
	do {                                                             \
		if (!(cond)) {                                           \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                      \
		}                                                        \
	} while (0)

/* ---- RAM disk ----------------------------------------------------------- */

#define SS 512UL
#define DISK_SECTORS 8192UL /* 4 MB */
static uint8_t disk[DISK_SECTORS * SS];
static uint8_t written[DISK_SECTORS]; /* sector was written */
static int disk_reads, disk_writes; /* commands */
static unsigned long last_read_lba, last_read_count;

static void sector_pattern(unsigned long s, uint8_t *out)
{
	for (unsigned long k = 0; k < SS; k++)
		out[k] = (uint8_t)(s * 7 + k * 13 + (s >> 8));
	memcpy(out, &s, sizeof(s));
}

static void disk_reset(void)
{
	for (unsigned long s = 0; s < DISK_SECTORS; s++)
		sector_pattern(s, disk + s * SS);
	memset(written, 0, sizeof(written));
	disk_reads = disk_writes = 0;
}

static int disk_read(block_device_t *dev, unsigned long lba,
		     unsigned long count, void *buf)
{
	(void)dev;
	if (lba == 0)
		test_fatal("read of LBA 0 (a hole read from the disk)", "disk");
	if (lba + count > DISK_SECTORS)
		test_fatal("read past the disk", "disk");
	memcpy(buf, disk + lba * SS, count * SS);
	disk_reads++;
	last_read_lba = lba;
	last_read_count = count;
	return ST_OK;
}

static int disk_fail_writes; /* every write fails while set */

static int disk_write(block_device_t *dev, unsigned long lba,
		      unsigned long count, const void *buf)
{
	(void)dev;
	if (disk_fail_writes)
		return ST_IO;
	if (lba == 0)
		test_fatal("write to LBA 0", "disk");
	if (lba + count > DISK_SECTORS)
		test_fatal("write past the disk", "disk");
	memcpy(disk + lba * SS, buf, count * SS);
	for (unsigned long i = 0; i < count; i++)
		written[lba + i] = 1;
	disk_writes++;
	return ST_OK;
}

static int sectors_written(void)
{
	int n = 0;

	for (unsigned long s = 0; s < DISK_SECTORS; s++)
		n += written[s];
	return n;
}

/* ---- the fake filesystem ------------------------------------------------ */

#define TBID_TAG (1UL << 63)
#define TBID_ENC(h, i) (TBID_TAG | ((unsigned long)(h) << 38) | (unsigned long)(i))
#define TBID_H(b) (((b) >> 38) & 0xFFFFFFUL)
#define TBID_I(b) ((b) & ((1UL << 38) - 1))
#define TEOC_MARKER 0xC000000000000000UL
#define TEOC 0xFFFFFFFFFFFFFFFFUL

#define NSTREAMS 8
#define MAXBLK 64

struct tstream {
	unsigned long nblocks;
	unsigned long lba[MAXBLK]; /* 0: hole / not placed yet */
	uint8_t reserved[MAXBLK]; /* delayed allocation pending */
};

struct tfs {
	vfs_superblock_t sb;
	block_device_t bdev;
	unsigned long bs;
	struct tstream st[NSTREAMS];
	int io_depth, map_depth;
	unsigned long next_lba; /* the hook's allocator */
	int hook_calls;
	unsigned long hook_chain[32], hook_first[32], hook_count[32];
	int hook_fail; /* ST_* to return, 0: succeed */
	int hook_bad_context;
	/* Invalidate this stream (chain id) when lock_io is next taken: what
	 * a truncate on another processor does while writeback, having
	 * taken the pages, waits for the lock. */
	unsigned long invalidate_on_lock;
};

static struct tfs *TFS(vfs_superblock_t *sb)
{
	return (struct tfs *)sb->fs_private;
}

static struct tstream *tstream_of(struct tfs *fs, unsigned long bid)
{
	if (!(bid & TBID_TAG) || TBID_H(bid) == 0 || TBID_H(bid) >= NSTREAMS)
		return 0;
	return &fs->st[TBID_H(bid)];
}

static unsigned long t_block_size(vfs_superblock_t *sb)
{
	return TFS(sb)->bs;
}
static const block_device_t *t_bdev(vfs_superblock_t *sb)
{
	return &TFS(sb)->bdev;
}
static unsigned long t_sector_size(vfs_superblock_t *sb)
{
	(void)sb;
	return SS;
}
static unsigned long t_block_to_lba(vfs_superblock_t *sb, unsigned long bid)
{
	struct tstream *s = tstream_of(TFS(sb), bid);

	if (!s || TBID_I(bid) >= s->nblocks)
		return 0;
	if (TFS(sb)->io_depth == 0 && TFS(sb)->map_depth == 0)
		test_fatal("block_to_lba without lock_map/lock_io", "tfs");
	return s->lba[TBID_I(bid)];
}
static unsigned long t_eoc(vfs_superblock_t *sb)
{
	(void)sb;
	return TEOC_MARKER;
}
static unsigned long t_next_block(vfs_superblock_t *sb, unsigned long bid)
{
	struct tstream *s = tstream_of(TFS(sb), bid);

	if (!s || TBID_I(bid) + 1 >= s->nblocks)
		return TEOC;
	return bid + 1;
}
static unsigned long t_map_block(vfs_superblock_t *sb, unsigned long chain,
				 unsigned long idx)
{
	struct tstream *s = tstream_of(TFS(sb), chain);

	if (!s || idx >= s->nblocks || !s->lba[idx])
		return 0;
	return TBID_ENC(TBID_H(chain), idx);
}
static unsigned long t_block_at(vfs_superblock_t *sb, unsigned long chain,
				unsigned long idx)
{
	struct tstream *s = tstream_of(TFS(sb), chain);

	if (!s || idx >= s->nblocks)
		return 0;
	return TBID_ENC(TBID_H(chain), idx);
}
static int t_write_inode(vfs_superblock_t *sb, struct ic_inode *ino)
{
	(void)sb;
	(void)ino;
	return 0;
}
static void t_lock_io(vfs_superblock_t *sb)
{
	if (test_locks_held)
		test_fatal("lock_io under a spinlock", "tfs");
	if (TFS(sb)->invalidate_on_lock) {
		unsigned long c = TFS(sb)->invalidate_on_lock;

		TFS(sb)->invalidate_on_lock = 0;
		pagecache_invalidate_file(sb, c);
	}
	TFS(sb)->io_depth++;
}
static void t_unlock_io(vfs_superblock_t *sb)
{
	if (TFS(sb)->io_depth <= 0)
		test_fatal("unlock_io not held", "tfs");
	TFS(sb)->io_depth--;
}
static void t_lock_map(vfs_superblock_t *sb)
{
	if (test_locks_held)
		test_fatal("lock_map under a spinlock", "tfs");
	TFS(sb)->map_depth++;
}
static void t_unlock_map(vfs_superblock_t *sb)
{
	if (TFS(sb)->map_depth <= 0)
		test_fatal("unlock_map not held", "tfs");
	TFS(sb)->map_depth--;
}
static unsigned long t_reserved_meta(vfs_superblock_t *sb)
{
	(void)sb;
	return 0;
}

static int t_alloc_blocks(vfs_superblock_t *sb, unsigned long chain,
			  unsigned long first, unsigned long count)
{
	struct tfs *fs = TFS(sb);
	struct tstream *s = tstream_of(fs, chain);
	unsigned long spb = fs->bs / SS;
	int placed = 0;

	if (fs->io_depth <= 0 || test_locks_held || !(test_irq_flag & 0x200))
		fs->hook_bad_context++;
	if (fs->hook_calls < 32) {
		fs->hook_chain[fs->hook_calls] = chain;
		fs->hook_first[fs->hook_calls] = first;
		fs->hook_count[fs->hook_calls] = count;
	}
	fs->hook_calls++;
	if (!s || count == 0)
		return ST_INVALID;
	if (fs->hook_fail)
		return fs->hook_fail;
	for (unsigned long i = first; i < first + count && i < s->nblocks;
	     i++) {
		if (s->reserved[i] && !s->lba[i]) {
			s->lba[i] = fs->next_lba;
			fs->next_lba += spb;
			s->reserved[i] = 0;
			placed++;
		}
	}
	return placed ? 0 : VFS_SB_ALLOC_NONE;
}

static const vfs_sb_ops_t t_ops_hook = {
	.block_size = t_block_size,
	.bdev = t_bdev,
	.sector_size = t_sector_size,
	.block_to_lba = t_block_to_lba,
	.end_of_chain_marker = t_eoc,
	.next_block = t_next_block,
	.map_block = t_map_block,
	.block_at = t_block_at,
	.write_inode = t_write_inode,
	.lock_io = t_lock_io,
	.unlock_io = t_unlock_io,
	.lock_map = t_lock_map,
	.unlock_map = t_unlock_map,
	.reserved_meta_block = t_reserved_meta,
	.alloc_blocks = t_alloc_blocks,
};

/* The same filesystem without delayed allocation (the shape of ext4). */
static const vfs_sb_ops_t t_ops_plain = {
	.block_size = t_block_size,
	.bdev = t_bdev,
	.sector_size = t_sector_size,
	.block_to_lba = t_block_to_lba,
	.end_of_chain_marker = t_eoc,
	.next_block = t_next_block,
	.map_block = t_map_block,
	.block_at = t_block_at,
	.write_inode = t_write_inode,
	.lock_io = t_lock_io,
	.unlock_io = t_unlock_io,
	.lock_map = t_lock_map,
	.unlock_map = t_unlock_map,
	.reserved_meta_block = t_reserved_meta,
};

static void tfs_init(struct tfs *fs, unsigned long bs, const vfs_sb_ops_t *ops)
{
	memset(fs, 0, sizeof(*fs));
	fs->bs = bs;
	fs->sb.ops = ops;
	fs->sb.fs_private = fs;
	fs->bdev.name = "ram0";
	fs->bdev.sector_size = SS;
	fs->bdev.total_sectors = DISK_SECTORS;
	fs->bdev.read = disk_read;
	fs->bdev.write = disk_write;
	fs->next_lba = 6000;
}

static void tfs_done(struct tfs *fs)
{
	pagecache_invalidate_sb(&fs->sb);
	CHECK(fs->io_depth == 0 && fs->map_depth == 0);
}

static unsigned long CID(unsigned h)
{
	return TBID_ENC(h, 0);
}

/* What page `pi' of stream `h' must read as: each block from its LBA, holes
 * and blocks past the end as zeros, bytes past `size' as zeros. */
static void expected_page(struct tfs *fs, unsigned h, unsigned long pi,
			  unsigned long size, uint8_t *out)
{
	struct tstream *s = &fs->st[h];
	unsigned long bs = fs->bs;

	memset(out, 0, PAGE_SIZE);
	for (unsigned long off = 0; off < PAGE_SIZE; off += SS) {
		unsigned long byte = pi * PAGE_SIZE + off;
		unsigned long b = byte / bs;
		if (b >= s->nblocks || !s->lba[b])
			continue;
		unsigned long sec = s->lba[b] + (byte % bs) / SS;
		memcpy(out + off, disk + sec * SS, SS);
	}
	if (pi * PAGE_SIZE + PAGE_SIZE > size) {
		unsigned long v = size > pi * PAGE_SIZE ? size - pi * PAGE_SIZE : 0;
		memset(out + v, 0, PAGE_SIZE - v);
	}
}

static int check_read(struct tfs *fs, unsigned h, unsigned long pi,
		      unsigned long size)
{
	static uint8_t want[PAGE_SIZE];
	pc_page_t *pg = pagecache_get_held(CID(h), pi, size, &fs->sb, CID(h));
	int ok;

	if (!pg) {
		printf("  read of stream %u page %lu failed\n", h, pi);
		return 0;
	}
	expected_page(fs, h, pi, size, want);
	ok = memcmp(pg->data, want, PAGE_SIZE) == 0;
	if (!ok)
		printf("  stream %u page %lu reads wrong data\n", h, pi);
	pagecache_put(pg);
	return ok;
}

static void fill(uint8_t *buf, unsigned long seed)
{
	for (unsigned long i = 0; i < PAGE_SIZE; i++)
		buf[i] = (uint8_t)(seed * 31 + i * 7 + (i >> 9));
}

/* Dirty page `pi' of stream `h' with fill(seed), as a write() would. */
static void dirty_page(struct tfs *fs, unsigned h, unsigned long pi,
		       unsigned long size, unsigned long seed)
{
	pc_page_t *pg = pagecache_get_for_write(CID(h), pi, size, &fs->sb,
						CID(h), 1);

	CHECK(pg != 0);
	if (!pg)
		return;
	fill(pg->data, seed);
	pagecache_write_end(pg);
}

static uint32_t page_flags(struct tfs *fs, unsigned h, unsigned long pi)
{
	pc_page_t *pg = pagecache_lookup_held(&fs->sb, CID(h), pi);
	uint32_t f;

	if (!pg)
		return 0xFFFFFFFFu;
	f = __atomic_load_n(&pg->flags, __ATOMIC_ACQUIRE);
	pagecache_put(pg);
	return f;
}

/* Block `b' of stream `h' holds bytes [boff, boff + len) of fill(seed)'s
 * page, starting at page offset `poff'. */
static int on_disk(struct tfs *fs, unsigned h, unsigned long b,
		   unsigned long seed, unsigned long poff)
{
	static uint8_t want[PAGE_SIZE];
	unsigned long lba = fs->st[h].lba[b];

	if (!lba)
		return 0;
	fill(want, seed);
	return memcmp(disk + lba * SS, want + poff, fs->bs < PAGE_SIZE ? fs->bs : PAGE_SIZE) == 0;
}

/* ---- 1. install / lookup_held ------------------------------------------- */

static void test_install(void)
{
	struct tfs fs;
	uint8_t src[PAGE_SIZE];
	long pages0 = test_pages;
	int w0;

	printf("install / lookup_held\n");
	tfs_init(&fs, 4096, &t_ops_plain);
	fs.st[1].nblocks = 4;

	CHECK(pagecache_lookup_held(&fs.sb, CID(1), 0) == 0);

	fill(src, 1);
	pc_page_t *a = pagecache_install(&fs.sb, CID(1), 0, src, 100, 1);
	CHECK(a != 0);
	if (!a)
		return;
	CHECK(memcmp(a->data, src, 100) == 0);
	{
		int zero = 1;
		for (unsigned long i = 100; i < PAGE_SIZE; i++)
			if (a->data[i])
				zero = 0;
		CHECK(zero);
	}
	CHECK(a->flags & PC_PAGE_VALID);
	CHECK(!(a->flags & PC_PAGE_DIRTY));
	CHECK(refcount_read(&a->refs) == 2); /* cache + caller */
	CHECK(test_pages == pages0 + 1);

	pc_page_t *b = pagecache_lookup_held(&fs.sb, CID(1), 0);
	CHECK(b == a);
	CHECK(refcount_read(&a->refs) == 3);
	pagecache_put(b);

	/* The key is cached: the cached copy wins, the new page is freed. */
	fill(src, 2);
	pc_page_t *c = pagecache_install(&fs.sb, CID(1), 0, src, PAGE_SIZE, 1);
	CHECK(c == a);
	CHECK(refcount_read(&a->refs) == 3);
	CHECK(a->data[0] != src[0] || a->data[1] != src[1]);
	CHECK(test_pages == pages0 + 1);
	pagecache_put(c);

	/* Not held. */
	pc_page_t *d = pagecache_install(&fs.sb, CID(1), 1, src, PAGE_SIZE, 0);
	CHECK(d != 0 && refcount_read(&d->refs) == 1);
	CHECK(d && memcmp(d->data, src, PAGE_SIZE) == 0);

	/* A zero page from no data at all. */
	pc_page_t *z = pagecache_install(&fs.sb, CID(1), 2, 0, 0, 1);
	CHECK(z != 0);
	if (z) {
		int zero = 1;
		for (unsigned long i = 0; i < PAGE_SIZE; i++)
			if (z->data[i])
				zero = 0;
		CHECK(zero);
		pagecache_put(z);
	}

	/* Misuse is refused, not half done. */
	w0 = test_warnings;
	CHECK(pagecache_install(&fs.sb, CID(1), 3, src, PAGE_SIZE + 1, 1) == 0);
	CHECK(test_warnings == w0 + 1);
	CHECK(pagecache_install(&fs.sb, 1, 3, src, 10, 1) == 0); /* id < 2 */
	CHECK(test_pages == pages0 + 3);

	/* pagecache_get finds an installed page without I/O. */
	disk_reads = 0;
	pc_page_t *g = pagecache_get_held(CID(1), 1, 4 * PAGE_SIZE, &fs.sb,
					  CID(1));
	CHECK(g == d);
	CHECK(disk_reads == 0);
	pagecache_put(g);

	/* Invalidated while held: detached at once, freed by the last put. */
	pagecache_invalidate_file(&fs.sb, CID(1));
	CHECK(pagecache_lookup_held(&fs.sb, CID(1), 0) == 0);
	CHECK(a->flags & PC_PAGE_DEAD);
	CHECK(test_pages == pages0 + 1); /* only the held one is left */
	pagecache_put(a);
	CHECK(test_pages == pages0);

	tfs_done(&fs);
}

/* ---- 2. coalesced read, blocks smaller than a page ----------------------- */

static void test_coalesced_read(void)
{
	struct tfs fs;
	struct tstream *s;

	printf("coalesced read with scrambled mappings\n");
	disk_reset();

	/* 1 KB blocks: four per page. */
	tfs_init(&fs, 1024, &t_ops_plain);
	s = &fs.st[1];
	s->nblocks = 18; /* 17 KB + : five pages, the last one partly past EOF */
	static const unsigned long map1[18] = {
		100, 50,  52,  200, /* page 0: scrambled */
		300, 302, 304, 306, /* page 1: contiguous */
		0,   0,   0,   0, /* page 2: all hole */
		400, 0,   404, 406, /* page 3: one hole */
		500, 502, /* page 4: two blocks, then the end */
	};
	memcpy(s->lba, map1, sizeof(map1));
	unsigned long size1 = 17 * 1024 + 100;
	for (unsigned long pi = 0; pi < 5; pi++)
		CHECK(check_read(&fs, 1, pi, size1));

	/* Reverse order: no read-ahead can paper over a wrong mapping. */
	pagecache_invalidate_file(&fs.sb, CID(1));
	for (unsigned long pi = 5; pi-- > 0;)
		CHECK(check_read(&fs, 1, pi, size1));

	/* A run of contiguous pages is still one device command. */
	s = &fs.st[2];
	s->nblocks = 32;
	for (unsigned long b = 0; b < 32; b++)
		s->lba[b] = 1000 + b * 2;
	disk_reads = 0;
	CHECK(check_read(&fs, 2, 0, 32 * 1024));
	CHECK(disk_reads == 1 && last_read_lba == 1000 &&
	      last_read_count == 64);
	for (unsigned long pi = 1; pi < 8; pi++)
		CHECK(check_read(&fs, 2, pi, 32 * 1024));
	CHECK(disk_reads == 1); /* the rest came with the first */

	/* Contiguous pages that are not adjacent on the disk: two runs. */
	s = &fs.st[3];
	s->nblocks = 8;
	for (unsigned long b = 0; b < 4; b++)
		s->lba[b] = 1200 + b * 2;
	for (unsigned long b = 4; b < 8; b++)
		s->lba[b] = 1400 + (b - 4) * 2;
	disk_reads = 0;
	CHECK(check_read(&fs, 3, 0, 8 * 1024));
	CHECK(check_read(&fs, 3, 1, 8 * 1024));
	CHECK(disk_reads == 2);

	/* A run stops at a page whose blocks are scrambled. */
	s = &fs.st[4];
	s->nblocks = 12;
	static const unsigned long map4[12] = { 1600, 1602, 1604, 1606,
						1608, 1650, 1612, 1614,
						1616, 1618, 1620, 1622 };
	memcpy(s->lba, map4, sizeof(map4));
	for (unsigned long pi = 0; pi < 3; pi++)
		CHECK(check_read(&fs, 4, pi, 12 * 1024));
	tfs_done(&fs);

	/* 512-byte blocks (eight per page) and 2 KB blocks (two). */
	tfs_init(&fs, 512, &t_ops_plain);
	s = &fs.st[1];
	s->nblocks = 16;
	static const unsigned long map5[16] = { 2000, 2001, 2002, 2003,
						2004, 2005, 2006, 2100,
						0,    0,    0,    0,
						0,    0,    0,    0 };
	memcpy(s->lba, map5, sizeof(map5));
	CHECK(check_read(&fs, 1, 0, 16 * 512));
	CHECK(check_read(&fs, 1, 1, 16 * 512));
	tfs_done(&fs);

	tfs_init(&fs, 2048, &t_ops_plain);
	s = &fs.st[1];
	s->nblocks = 6;
	static const unsigned long map6[6] = { 2200, 2300, 2400, 2404, 0, 2500 };
	memcpy(s->lba, map6, sizeof(map6));
	for (unsigned long pi = 0; pi < 3; pi++)
		CHECK(check_read(&fs, 1, pi, 6 * 2048));
	tfs_done(&fs);

	/* Blocks larger than a page: every page of a hole block is zeros
	 * (not the sectors after LBA 0). */
	tfs_init(&fs, 8192, &t_ops_plain);
	s = &fs.st[1];
	s->nblocks = 3;
	s->lba[0] = 2600;
	s->lba[1] = 0;
	s->lba[2] = 2700;
	for (unsigned long pi = 0; pi < 6; pi++)
		CHECK(check_read(&fs, 1, pi, 3 * 8192));
	tfs_done(&fs);

	CHECK(sectors_written() == 0);
}

/* ---- 3. delayed allocation, one block per page --------------------------- */

static void test_delalloc_page_blocks(void)
{
	struct tfs fs;
	struct tstream *s;
	int w0;

	printf("delayed allocation, 4 KB blocks\n");
	disk_reset();
	tfs_init(&fs, 4096, &t_ops_hook);

	/* Eight unplaced pages: ONE hook call for the run, one command. */
	s = &fs.st[1];
	s->nblocks = 8;
	for (int b = 0; b < 8; b++)
		s->reserved[b] = 1;
	for (unsigned long pi = 0; pi < 8; pi++)
		dirty_page(&fs, 1, pi, 8 * PAGE_SIZE, 100 + pi);
	disk_writes = 0;
	CHECK(pagecache_flush_file(&fs.sb, CID(1)) == 8);
	CHECK(fs.hook_calls == 1);
	CHECK(fs.hook_chain[0] == CID(1) && fs.hook_first[0] == 0 &&
	      fs.hook_count[0] == 8);
	CHECK(disk_writes == 1);
	for (unsigned long b = 0; b < 8; b++) {
		CHECK(s->lba[b] == 6000 + b * 8);
		CHECK(on_disk(&fs, 1, b, 100 + b, 0));
		CHECK(!(page_flags(&fs, 1, b) & (PC_PAGE_DIRTY | PC_PAGE_LOCKED)));
	}
	/* And it reads back. */
	pagecache_invalidate_file(&fs.sb, CID(1));
	for (unsigned long pi = 0; pi < 8; pi++)
		CHECK(check_read(&fs, 1, pi, 8 * PAGE_SIZE));

	/* Mixed: placed, unplaced, placed, unplaced -> one call per run. */
	fs.hook_calls = 0;
	s = &fs.st[2];
	s->nblocks = 6;
	s->lba[0] = 3000;
	s->lba[1] = 3008;
	s->reserved[2] = s->reserved[3] = 1;
	s->lba[4] = 3100;
	s->reserved[5] = 1;
	for (unsigned long pi = 0; pi < 6; pi++)
		dirty_page(&fs, 2, pi, 6 * PAGE_SIZE, 200 + pi);
	CHECK(pagecache_flush_file(&fs.sb, CID(2)) == 6);
	CHECK(fs.hook_calls == 2);
	CHECK(fs.hook_first[0] == 2 && fs.hook_count[0] == 2);
	CHECK(fs.hook_first[1] == 5 && fs.hook_count[1] == 1);
	for (unsigned long b = 0; b < 6; b++)
		CHECK(on_disk(&fs, 2, b, 200 + b, 0));

	/* The hook fails: nothing written, the pages stay dirty, no hold is
	 * leaked, it is reported; the next pass stores them. */
	fs.hook_calls = 0;
	s = &fs.st[3];
	s->nblocks = 2;
	s->reserved[0] = s->reserved[1] = 1;
	dirty_page(&fs, 3, 0, 2 * PAGE_SIZE, 300);
	dirty_page(&fs, 3, 1, 2 * PAGE_SIZE, 301);
	fs.hook_fail = ST_NOSPC;
	w0 = test_warnings;
	int sec0 = sectors_written();
	CHECK(pagecache_flush_file(&fs.sb, CID(3)) == 0);
	CHECK(fs.hook_calls == 1);
	CHECK(test_warnings == w0 + 1);
	CHECK(sectors_written() == sec0);
	for (unsigned long pi = 0; pi < 2; pi++) {
		uint32_t f = page_flags(&fs, 3, pi);
		CHECK((f & PC_PAGE_DIRTY) && !(f & PC_PAGE_LOCKED));
		pc_page_t *pg = pagecache_lookup_held(&fs.sb, CID(3), pi);
		CHECK(pg && refcount_read(&pg->refs) == 2);
		pagecache_put(pg);
	}
	/* Through every other writeback entry point too. */
	CHECK(pagecache_flush_sb(&fs.sb) == 0);
	CHECK(pagecache_flush_all() == 0);
	CHECK(pagecache_flush_bounded(64) == 0);
	CHECK(sectors_written() == sec0);
	CHECK((page_flags(&fs, 3, 0) & PC_PAGE_DIRTY) &&
	      (page_flags(&fs, 3, 1) & PC_PAGE_DIRTY));
	fs.hook_fail = 0;
	CHECK(pagecache_flush_sb(&fs.sb) == 2);
	CHECK(on_disk(&fs, 3, 0, 300, 0) && on_disk(&fs, 3, 1, 301, 0));
	CHECK(!(page_flags(&fs, 3, 0) & PC_PAGE_DIRTY));

	/* Nothing reserved (a dirty page over a genuine hole, which the
	 * filesystem should never produce): VFS_SB_ALLOC_NONE, it stays
	 * dirty, and that is said. */
	fs.hook_calls = 0;
	s = &fs.st[4];
	s->nblocks = 1;
	dirty_page(&fs, 4, 0, PAGE_SIZE, 400);
	w0 = test_warnings;
	sec0 = sectors_written();
	CHECK(pagecache_flush_file(&fs.sb, CID(4)) == 0);
	CHECK(fs.hook_calls == 1);
	CHECK(test_warnings == w0 + 1);
	CHECK(sectors_written() == sec0);
	CHECK(page_flags(&fs, 4, 0) & PC_PAGE_DIRTY);
	pagecache_invalidate_file(&fs.sb, CID(4));

	/* flush_all and the bounded flush reach the hook as well. */
	fs.hook_calls = 0;
	s = &fs.st[5];
	s->nblocks = 3;
	s->reserved[0] = s->reserved[1] = s->reserved[2] = 1;
	for (unsigned long pi = 0; pi < 3; pi++)
		dirty_page(&fs, 5, pi, 3 * PAGE_SIZE, 500 + pi);
	CHECK(pagecache_flush_all() == 3);
	CHECK(fs.hook_calls == 1 && fs.hook_count[0] == 3);
	for (unsigned long b = 0; b < 3; b++)
		CHECK(on_disk(&fs, 5, b, 500 + b, 0));

	/* Reclaim's own writeback (pagecache_shrink with flush_dirty). */
	fs.hook_calls = 0;
	s = &fs.st[6];
	s->nblocks = 1;
	s->reserved[0] = 1;
	dirty_page(&fs, 6, 0, PAGE_SIZE, 600);
	{
		pc_page_t *pg = pagecache_lookup_held(&fs.sb, CID(6), 0);
		__atomic_fetch_and(&pg->flags, ~PC_PAGE_REFERENCED,
				   __ATOMIC_ACQ_REL);
		pagecache_put(pg);
	}
	pagecache_shrink(1, 1);
	CHECK(fs.hook_calls == 1);
	CHECK(on_disk(&fs, 6, 0, 600, 0));

	CHECK(fs.hook_bad_context == 0);
	tfs_done(&fs);

	/* Without the hook nothing changes: a dirty page over a hole stays
	 * dirty, silently, and is never written anywhere. */
	printf("no hook: unchanged behaviour\n");
	tfs_init(&fs, 4096, &t_ops_plain);
	s = &fs.st[1];
	s->nblocks = 2;
	s->lba[0] = 3500;
	dirty_page(&fs, 1, 0, 2 * PAGE_SIZE, 700);
	dirty_page(&fs, 1, 1, 2 * PAGE_SIZE, 701);
	w0 = test_warnings;
	sec0 = sectors_written();
	CHECK(pagecache_flush_file(&fs.sb, CID(1)) == 1);
	CHECK(test_warnings == w0);
	CHECK(sectors_written() == sec0 + 8);
	CHECK(on_disk(&fs, 1, 0, 700, 0));
	CHECK(page_flags(&fs, 1, 1) & PC_PAGE_DIRTY);
	tfs_done(&fs);
}

/* ---- 4. delayed allocation, blocks smaller than a page ------------------- */

static void test_delalloc_small_blocks(void)
{
	struct tfs fs;
	struct tstream *s;
	int w0, sec0;

	printf("delayed allocation, 1 KB blocks\n");
	disk_reset();
	tfs_init(&fs, 1024, &t_ops_hook);

	/* Two pages, all reserved: one call per page, every block stored. */
	s = &fs.st[1];
	s->nblocks = 8;
	for (int b = 0; b < 8; b++)
		s->reserved[b] = 1;
	dirty_page(&fs, 1, 0, 8 * 1024, 10);
	dirty_page(&fs, 1, 1, 8 * 1024, 11);
	CHECK(pagecache_flush_file(&fs.sb, CID(1)) == 2);
	CHECK(fs.hook_calls == 2);
	CHECK(fs.hook_count[0] == 4 && fs.hook_count[1] == 4);
	CHECK((fs.hook_first[0] == 0 && fs.hook_first[1] == 4) ||
	      (fs.hook_first[0] == 4 && fs.hook_first[1] == 0));
	for (unsigned long b = 0; b < 8; b++)
		CHECK(on_disk(&fs, 1, b, 10 + b / 4, (b % 4) * 1024));
	pagecache_invalidate_file(&fs.sb, CID(1));
	CHECK(check_read(&fs, 1, 0, 8 * 1024));
	CHECK(check_read(&fs, 1, 1, 8 * 1024));

	/* Half reserved, half genuine hole: the reserved blocks are placed and
	 * stored, the holes skipped (never LBA 0), the page clean. */
	fs.hook_calls = 0;
	s = &fs.st[2];
	s->nblocks = 4;
	s->reserved[0] = s->reserved[1] = 1;
	{
		pc_page_t *pg = pagecache_get_for_write(CID(2), 0, 4 * 1024,
							&fs.sb, CID(2), 1);
		CHECK(pg != 0);
		fill(pg->data, 20);
		memset(pg->data + 2048, 0, 2048);
		pagecache_write_end(pg);
	}
	sec0 = sectors_written();
	CHECK(pagecache_flush_file(&fs.sb, CID(2)) == 1);
	CHECK(fs.hook_calls == 1 && fs.hook_first[0] == 0 &&
	      fs.hook_count[0] == 4);
	CHECK(s->lba[0] && s->lba[1] && !s->lba[2] && !s->lba[3]);
	CHECK(sectors_written() == sec0 + 4);
	CHECK(on_disk(&fs, 2, 0, 20, 0) && on_disk(&fs, 2, 1, 20, 1024));
	CHECK(!(page_flags(&fs, 2, 0) & PC_PAGE_DIRTY));

	/* Failure: dirty, nothing written, through flush_file and flush_all
	 * (the per-page paths), then stored once the hook works. */
	fs.hook_calls = 0;
	s = &fs.st[3];
	s->nblocks = 4;
	for (int b = 0; b < 4; b++)
		s->reserved[b] = 1;
	dirty_page(&fs, 3, 0, 4 * 1024, 30);
	fs.hook_fail = ST_IO;
	w0 = test_warnings;
	sec0 = sectors_written();
	CHECK(pagecache_flush_file(&fs.sb, CID(3)) == 0);
	CHECK(pagecache_flush_all() == 0);
	CHECK(fs.hook_calls == 2);
	CHECK(test_warnings == w0 + 2);
	CHECK(sectors_written() == sec0);
	{
		uint32_t f = page_flags(&fs, 3, 0);
		CHECK((f & PC_PAGE_DIRTY) && !(f & PC_PAGE_LOCKED));
	}
	fs.hook_fail = 0;
	CHECK(pagecache_flush_all() == 1);
	for (unsigned long b = 0; b < 4; b++)
		CHECK(on_disk(&fs, 3, b, 30, b * 1024));

	/* Reclaim's writeback, per-page path. */
	fs.hook_calls = 0;
	s = &fs.st[4];
	s->nblocks = 4;
	for (int b = 0; b < 4; b++)
		s->reserved[b] = 1;
	dirty_page(&fs, 4, 0, 4 * 1024, 40);
	{
		pc_page_t *pg = pagecache_lookup_held(&fs.sb, CID(4), 0);
		__atomic_fetch_and(&pg->flags, ~PC_PAGE_REFERENCED,
				   __ATOMIC_ACQ_REL);
		pagecache_put(pg);
	}
	pagecache_shrink(1, 1);
	CHECK(fs.hook_calls == 1);
	for (unsigned long b = 0; b < 4; b++)
		CHECK(on_disk(&fs, 4, b, 40, b * 1024));

	CHECK(fs.hook_bad_context == 0);
	tfs_done(&fs);
}

/* ---- 5. write errors and invalidated pages on the per-page paths -------- */

/* Blocks of `bs' bytes (not a page): a failed write leaves the page dirty
 * and uncounted on flush_file, flush_all and reclaim, and it is stored once
 * the device works again; a page invalidated while writeback waited for
 * the I/O lock is never written. */
static void test_wb_errors_bs(unsigned long bs)
{
	struct tfs fs;
	struct tstream *s;
	unsigned long spb = bs / SS, nblk = bs < PAGE_SIZE ? PAGE_SIZE / bs : 1;
	int sec0;

	printf("write errors and invalidated pages, %lu-byte blocks\n", bs);
	disk_reset();
	tfs_init(&fs, bs, &t_ops_plain);
	s = &fs.st[1];
	s->nblocks = nblk;
	for (unsigned long b = 0; b < nblk; b++)
		s->lba[b] = 1000 + b * spb * 3; /* scattered */

	/* Failing device: nothing counted, the page stays dirty. */
	dirty_page(&fs, 1, 0, nblk * bs, 50);
	disk_fail_writes = 1;
	CHECK(pagecache_flush_file(&fs.sb, CID(1)) == 0);
	CHECK(page_flags(&fs, 1, 0) & PC_PAGE_DIRTY);
	CHECK(pagecache_flush_all() == 0);
	CHECK(page_flags(&fs, 1, 0) & PC_PAGE_DIRTY);
	{
		pc_page_t *pg = pagecache_lookup_held(&fs.sb, CID(1), 0);
		__atomic_fetch_and(&pg->flags, ~PC_PAGE_REFERENCED,
				   __ATOMIC_ACQ_REL);
		pagecache_put(pg);
	}
	pagecache_shrink(1, 1);
	CHECK(page_flags(&fs, 1, 0) & PC_PAGE_DIRTY);
	CHECK(!(page_flags(&fs, 1, 0) & PC_PAGE_LOCKED));

	/* Device back: stored, clean. */
	disk_fail_writes = 0;
	CHECK(pagecache_flush_file(&fs.sb, CID(1)) == 1);
	CHECK(!(page_flags(&fs, 1, 0) & PC_PAGE_DIRTY));
	if (bs < PAGE_SIZE)
		for (unsigned long b = 0; b < nblk; b++)
			CHECK(on_disk(&fs, 1, b, 50, b * bs));
	else {
		static uint8_t want[PAGE_SIZE];

		fill(want, 50);
		CHECK(memcmp(disk + s->lba[0] * SS, want, PAGE_SIZE) == 0);
	}

	/* Invalidated while writeback (flush_file) waited for lock_io:
	 * nothing written. */
	dirty_page(&fs, 1, 0, nblk * bs, 51);
	sec0 = sectors_written();
	fs.invalidate_on_lock = CID(1);
	CHECK(pagecache_flush_file(&fs.sb, CID(1)) == 0);
	CHECK(sectors_written() == sec0);
	CHECK(page_flags(&fs, 1, 0) == 0xFFFFFFFFu); /* gone */

	/* ... through flush_all ... */
	dirty_page(&fs, 1, 0, nblk * bs, 52);
	sec0 = sectors_written();
	fs.invalidate_on_lock = CID(1);
	CHECK(pagecache_flush_all() == 0);
	CHECK(sectors_written() == sec0);

	/* ... and through reclaim. */
	dirty_page(&fs, 1, 0, nblk * bs, 53);
	{
		pc_page_t *pg = pagecache_lookup_held(&fs.sb, CID(1), 0);
		__atomic_fetch_and(&pg->flags, ~PC_PAGE_REFERENCED,
				   __ATOMIC_ACQ_REL);
		pagecache_put(pg);
	}
	sec0 = sectors_written();
	fs.invalidate_on_lock = CID(1);
	pagecache_shrink(1, 1);
	CHECK(sectors_written() == sec0);
	tfs_done(&fs);
}

int main(void)
{
	pagecache_init();
	test_install();
	test_coalesced_read();
	test_delalloc_page_blocks();
	test_delalloc_small_blocks();
	test_wb_errors_bs(1024);
	test_wb_errors_bs(2048);
	test_wb_errors_bs(8192);

	CHECK(test_locks_held == 0);
	CHECK(test_irq_flag == 0x200);
	CHECK(test_pages == 0);
	if (failures) {
		printf("test-pagecache-ext: %d FAILED\n", failures);
		exit(1);
	}
	printf("test-pagecache-ext: all passed\n");
	return 0;
}
