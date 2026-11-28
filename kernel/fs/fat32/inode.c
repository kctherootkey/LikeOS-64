/* LikeOS - FAT filesystem driver: the boot sector, streams (the in-memory
 * inodes), page-cache superblock operations, stat, statfs, sync, the
 * operations table, mount options, mount, unmount, remount and probe
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* The root superblock of the caches' whole-system flushes (pagecache_sync).
 * Defined here for historical reasons: the root filesystem was FAT first;
 * ext4 sets it when it is the root. */
vfs_superblock_t *g_root_sb = 0;

static const char *const fat_ro_reason_text[] = {
	[FAT_RO_NONE] = "",
	[FAT_RO_REQUESTED] = "read-only requested",
	[FAT_RO_DEVICE] = "the device is write-protected",
	[FAT_RO_BACKUP_BOOT] =
		"main boot sector unusable, mounted from the backup boot sector",
	[FAT_RO_VERSION] = "unsupported FAT32 version",
	[FAT_RO_GEOMETRY] = "the volume is larger than the device",
	[FAT_RO_ERRORS] = "file system errors (errors=remount-ro)",
	[FAT_RO_FAT_MEDIA] = "the first FAT entry holds no media byte",
};
static fat_stream_t *fat_stream_by_handle(fat_fs_t *fs, uint32_t h);

/* ===========================================================================
 * 6. The boot sector
 *
 * The BIOS parameter block is checked field by field the way the
 * specification defines it; every derived quantity is computed in 64 bits
 * and checked against the volume and the FAT.  The FAT type follows the
 * specification: a 16-bit FAT size of 0 means FAT32, otherwise the cluster
 * count decides (FAT12 up to 4084 clusters).  Nothing here writes, except
 * fat_boot_write_state() and fat_fsinfo_write(), which the commit machinery
 * calls on an armed mount.
 * =========================================================================== */

/* The fields of a boot sector this driver uses, parsed and checked. */
struct fat_geo {
	unsigned bps; /* bytes per logical sector */
	unsigned spc; /* sectors per cluster */
	unsigned reserved;
	unsigned nfats;
	unsigned root_entries;
	unsigned media;
	uint64_t total_sectors;
	uint64_t fat_length; /* sectors of one FAT */
	uint64_t root_dir_sectors;
	uint64_t data_start; /* first sector of cluster 2 */
	uint64_t cluster_count;
	uint32_t root_cluster; /* FAT32 */
	unsigned fsinfo; /* FAT32: sector, 0 none */
	unsigned backup; /* FAT32: sector, 0 none */
	unsigned extflags; /* FAT32 */
	unsigned version; /* FAT32 */
	int has_ext; /* extended boot signature present */
	unsigned state_off; /* offset of the state byte (has_ext) */
	uint8_t state;
	uint32_t serial;
	uint8_t label[11];
	int fat_bits;
	int dos1x; /* built from a DOS 1.x floppy's defaults */
};

static int fat_valid_media(unsigned m)
{
	return m == 0xF0 || m >= 0xF8;
}

/* Does the boot sector start with a jump instruction (EB xx 90 or E9 xx xx),
 * as every formatter writes one? */
static int fat_has_jump(const uint8_t *b)
{
	return (b[0] == 0xEB && b[2] == 0x90) || b[0] == 0xE9;
}

/* Is `b' the boot sector of another file system that happens to carry a
 * plausible parameter block?  NTFS and exFAT name themselves. */
static int fat_other_fs(const uint8_t *b)
{
	static const char ntfs[8] = { 'N', 'T', 'F', 'S', ' ', ' ', ' ', ' ' };
	static const char exfat[8] = { 'E', 'X', 'F', 'A', 'T', ' ', ' ', ' ' };
	int n = 1, e = 1;

	for (int i = 0; i < 8; i++) {
		if (b[FAT_BS_OEM + i] != (uint8_t)ntfs[i])
			n = 0;
		if (b[FAT_BS_OEM + i] != (uint8_t)exfat[i])
			e = 0;
	}
	return n || e;
}

/* Parse a parameter block and derive the geometry.  `forced_bits' is the
 * fat= option (0: detect).  ST_OK, or ST_INVALID with a log line (unless
 * `quiet') naming the broken rule. */
static int fat_bpb_parse(const uint8_t *b, struct fat_geo *g, const char *dev,
			 int forced_bits, int quiet)
{
	uint64_t fat16 = fat_le16(b + FAT_BS_FATSZ16);
	uint64_t fat32 = fat_le32(b + FAT_BS32_FATSZ);
	uint64_t total, fat_area, fat_entries, max_cl;

#define FAT_BPB_BAD(fmt, ...)                                         \
	do {                                                          \
		if (!quiet)                                           \
			kprintf("fat: %s: " fmt, dev, ##__VA_ARGS__); \
		return ST_INVALID;                                    \
	} while (0)

	mm_memset(g, 0, sizeof(*g));
	if (fat_other_fs(b))
		return ST_INVALID;
	g->bps = fat_le16(b + FAT_BS_BPS);
	g->spc = b[FAT_BS_SPC];
	g->reserved = fat_le16(b + FAT_BS_RESERVED);
	g->nfats = b[FAT_BS_NFATS];
	g->root_entries = fat_le16(b + FAT_BS_ROOT_ENTRIES);
	g->media = b[FAT_BS_MEDIA];
	total = fat_le16(b + FAT_BS_TOTAL16);
	if (total == 0)
		total = fat_le32(b + FAT_BS_TOTAL32);
	g->total_sectors = total;

	if (!fat_is_pow2(g->bps) || g->bps < 512 || g->bps > 4096)
		FAT_BPB_BAD("bogus logical sector size %u\n", g->bps);
	if (!fat_is_pow2(g->spc))
		FAT_BPB_BAD("bogus sectors per cluster %u\n", g->spc);
	if (g->reserved == 0)
		FAT_BPB_BAD("bogus number of reserved sectors\n");
	if (g->nfats == 0 || g->nfats > FAT_MAX_FATS)
		FAT_BPB_BAD("bogus number of FATs %u\n", g->nfats);
	if (!fat_valid_media(g->media))
		FAT_BPB_BAD("invalid media value 0x%02x\n", g->media);
	if (fat16 == 0 && fat32 == 0)
		FAT_BPB_BAD("bogus number of FAT sectors\n");
	if (fat16 == 0) {
		g->fat_bits = 32;
		g->fat_length = fat32;
		g->root_cluster = fat_le32(b + FAT_BS32_ROOT);
		g->fsinfo = fat_le16(b + FAT_BS32_FSINFO);
		g->backup = fat_le16(b + FAT_BS32_BACKUP);
		g->extflags = fat_le16(b + FAT_BS32_EXTFLAGS);
		g->version = fat_le16(b + FAT_BS32_VERSION);
		if (b[FAT_BS32_SIG] == FAT_EXT_SIG ||
		    b[FAT_BS32_SIG] == FAT_EXT_SIG_OLD) {
			g->has_ext = 1;
			g->state_off = FAT_BS32_STATE;
			g->state = b[FAT_BS32_STATE];
			g->serial = fat_le32(b + FAT_BS32_SERIAL);
			if (b[FAT_BS32_SIG] == FAT_EXT_SIG)
				mm_memcpy(g->label, b + FAT_BS32_LABEL, 11);
		}
		if (g->root_entries != 0)
			FAT_BPB_BAD("FAT32 volume with %u fixed root entries\n",
				    g->root_entries);
	} else {
		g->fat_length = fat16;
		if (b[FAT_BS16_SIG] == FAT_EXT_SIG ||
		    b[FAT_BS16_SIG] == FAT_EXT_SIG_OLD) {
			g->has_ext = 1;
			g->state_off = FAT_BS16_STATE;
			g->state = b[FAT_BS16_STATE];
			g->serial = fat_le32(b + FAT_BS16_SERIAL);
			if (b[FAT_BS16_SIG] == FAT_EXT_SIG)
				mm_memcpy(g->label, b + FAT_BS16_LABEL, 11);
		}
		if (g->root_entries == 0)
			FAT_BPB_BAD("FAT12/16 volume without root directory entries\n");
	}
	/* The root directory region holds whole sectors of entries. */
	if (((uint64_t)g->root_entries * FAT_DENTRY_SIZE) & (g->bps - 1))
		FAT_BPB_BAD("bogus number of directory entries %u\n",
			    g->root_entries);
	g->root_dir_sectors =
		((uint64_t)g->root_entries * FAT_DENTRY_SIZE) / g->bps;
	fat_area = (uint64_t)g->nfats * g->fat_length;
	g->data_start = (uint64_t)g->reserved + fat_area + g->root_dir_sectors;
	if (total <= g->data_start)
		FAT_BPB_BAD("volume of %llu sectors has no data area (it starts at %llu)\n",
			    (unsigned long long)total,
			    (unsigned long long)g->data_start);
	g->cluster_count = (total - g->data_start) / g->spc;
	if (g->fat_bits != 32)
		g->fat_bits = g->cluster_count <= FAT12_MAX_CLUSTERS ? 12 : 16;
	if (forced_bits) {
		if ((forced_bits == 32) != (g->fat_bits == 32))
			FAT_BPB_BAD("fat=%d does not fit this volume\n",
				    forced_bits);
		g->fat_bits = forced_bits;
	}
	/* A FAT holds only so many entries: clusters past them do not exist. */
	fat_entries = (g->fat_length * g->bps * 8) / (uint64_t)g->fat_bits;
	if (fat_entries < 3)
		FAT_BPB_BAD("FAT of %llu sectors too small\n",
			    (unsigned long long)g->fat_length);
	if (g->cluster_count > fat_entries - 2)
		g->cluster_count = fat_entries - 2;
	max_cl = g->fat_bits == 12 ? FAT12_MAX_CLUSTERS :
		 g->fat_bits == 16 ? FAT16_MAX_CLUSTERS :
				     FAT32_MAX_CLUSTERS;
	if (g->cluster_count == 0)
		FAT_BPB_BAD("volume without clusters\n");
	if (g->cluster_count > max_cl)
		FAT_BPB_BAD("count of clusters too big (%llu) for FAT%d\n",
			    (unsigned long long)g->cluster_count, g->fat_bits);
	if (g->fat_bits == 32) {
		if (g->root_cluster < FAT_FIRST_CLUSTER ||
		    (uint64_t)g->root_cluster >= g->cluster_count + 2)
			FAT_BPB_BAD("root directory cluster %u out of range\n",
				    g->root_cluster);
		if ((g->extflags & FAT_EXTFLAGS_NOMIRROR) &&
		    (g->extflags & FAT_EXTFLAGS_ACTIVE) >= g->nfats)
			FAT_BPB_BAD("active FAT %u of %u\n",
				    g->extflags & FAT_EXTFLAGS_ACTIVE, g->nfats);
		/* FSInfo and the backup boot sector live in the reserved
		 * area; anything else is ignored. */
		if (g->fsinfo == 0 || g->fsinfo == 0xFFFF ||
		    g->fsinfo >= g->reserved)
			g->fsinfo = 0;
		if (g->backup == 0 || g->backup == 0xFFFF ||
		    g->backup >= g->reserved || g->backup == g->fsinfo)
			g->backup = 0;
	}
#undef FAT_BPB_BAD
	return ST_OK;
}

/* DOS 1.x floppies carry no parameter block: their geometry follows from
 * the device size.  Sizes in 512-byte sectors. */
static const struct {
	uint32_t sectors;
	uint8_t spc;
	uint16_t root_entries;
	uint8_t media;
	uint8_t fat_length;
} fat_floppy_defaults[] = {
	{ 320, 1, 64, 0xFE, 1 }, /* 160 KB, single sided, 8 sectors */
	{ 360, 1, 64, 0xFC, 2 }, /* 180 KB, single sided, 9 sectors */
	{ 640, 2, 112, 0xFF, 1 }, /* 320 KB, double sided, 8 sectors */
	{ 720, 2, 112, 0xFD, 2 }, /* 360 KB, double sided, 9 sectors */
};

/* The geometry of a DOS 1.x floppy (dos1xfloppy): a boot sector with a jump
 * and no parameter block, a device of one of the four sizes, and a FAT whose
 * first byte is that size's media byte.  ST_OK or ST_INVALID. */
static int fat_bpb_dos1x(fat_fs_t *fs, const uint8_t *b, struct fat_geo *g)
{
	uint64_t sectors512 = ((uint64_t)fs->bdev->total_sectors << fs->dev_shift) >>
			      9;
	uint8_t *sec;
	int k = -1, r;

	if (fs->dev_ss != 512 || !fat_has_jump(b))
		return ST_INVALID;
	for (unsigned i = 0;
	     i < sizeof(fat_floppy_defaults) / sizeof(fat_floppy_defaults[0]);
	     i++)
		if (fat_floppy_defaults[i].sectors == sectors512)
			k = (int)i;
	if (k < 0)
		return ST_INVALID;
	sec = (uint8_t *)kalloc(512);
	if (!sec)
		return ST_NOMEM;
	r = fat_dev_read(fs, 1, 1, sec);
	if (r == ST_OK && sec[0] != fat_floppy_defaults[k].media)
		r = ST_INVALID;
	kfree(sec);
	if (r != ST_OK)
		return r == ST_NOMEM ? r : ST_INVALID;
	mm_memset(g, 0, sizeof(*g));
	g->bps = 512;
	g->spc = fat_floppy_defaults[k].spc;
	g->reserved = 1;
	g->nfats = 2;
	g->root_entries = fat_floppy_defaults[k].root_entries;
	g->media = fat_floppy_defaults[k].media;
	g->total_sectors = fat_floppy_defaults[k].sectors;
	g->fat_length = fat_floppy_defaults[k].fat_length;
	g->root_dir_sectors = (uint64_t)g->root_entries * FAT_DENTRY_SIZE / 512;
	g->data_start = 1 + 2 * g->fat_length + g->root_dir_sectors;
	g->cluster_count = (g->total_sectors - g->data_start) / g->spc;
	g->fat_bits = 12;
	g->dos1x = 1;
	return ST_OK;
}

/* Read `bytes' (a multiple of nothing in particular) at byte offset `off'
 * of the device into a kalloc'd buffer of whole device sectors.  ST_INVALID
 * when the offset is not on a device sector. */
static int fat_read_bytes(fat_fs_t *fs, uint64_t off, unsigned long bytes,
			  uint8_t **out)
{
	uint64_t n = fat_round_up(bytes, fs->dev_ss);
	uint8_t *buf;
	int r;

	*out = NULL;
	if (off & (fs->dev_ss - 1))
		return ST_INVALID;
	buf = (uint8_t *)kalloc((size_t)n);
	if (!buf)
		return ST_NOMEM;
	r = fat_dev_read(fs, off >> fs->dev_shift, n >> fs->dev_shift, buf);
	if (r != ST_OK) {
		kfree(buf);
		return r;
	}
	*out = buf;
	return ST_OK;
}

/* Find the parameter block to mount from: the boot sector when it parses;
 * else, for FAT32, the backup boot sector (sector 6 in every formatter's
 * layout; read-only then); else a DOS 1.x floppy with dos1xfloppy.  Fills
 * *g and *from_backup.  The first 512 bytes of the boot sector are kept in
 * `boot' (for the probe's other-fs check). */
static int fat_boot_load(fat_fs_t *fs, struct fat_geo *g, int *from_backup,
			 int quiet)
{
	uint8_t *first = NULL, *bk = NULL;
	int r;

	*from_backup = 0;
	r = fat_read_bytes(fs, 0, 512, &first);
	if (r != ST_OK)
		return r;
	if (first[FAT_BS_SIGNATURE] == 0x55 &&
	    first[FAT_BS_SIGNATURE + 1] == 0xAA) {
		/* The usual case. */
	} else if (!fat_has_jump(first)) {
		/* Neither the signature nor a jump: no boot sector at all,
		 * unless it is one of the oldest floppies. */
		if (fs->opts.dos1xfloppy &&
		    fat_bpb_dos1x(fs, first, g) == ST_OK) {
			kfree(first);
			return ST_OK;
		}
		kfree(first);
		return ST_NOT_FOUND;
	}
	r = fat_bpb_parse(first, g, fs->bdev->name, fs->opts.fat_bits, quiet);
	if (r == ST_OK) {
		kfree(first);
		return ST_OK;
	}
	if (fs->opts.dos1xfloppy && fat_bpb_dos1x(fs, first, g) == ST_OK) {
		kfree(first);
		return ST_OK;
	}
	if (fat_other_fs(first)) {
		kfree(first);
		return ST_NOT_FOUND;
	}
	kfree(first);
	/* The FAT32 backup boot sector at logical sector 6, for every legal
	 * logical sector size that lands on a device sector. */
	for (unsigned shift = 9; shift <= 12; shift++) {
		uint64_t off = (uint64_t)6 << shift;

		if (off & (fs->dev_ss - 1))
			continue;
		if (fat_read_bytes(fs, off, 512, &bk) != ST_OK)
			continue;
		if (bk[FAT_BS_SIGNATURE] == 0x55 &&
		    bk[FAT_BS_SIGNATURE + 1] == 0xAA &&
		    fat_bpb_parse(bk, g, fs->bdev->name, fs->opts.fat_bits, 1) ==
			    ST_OK &&
		    g->fat_bits == 32 && g->bps == (1u << shift) &&
		    g->backup == 6) {
			kfree(bk);
			*from_backup = 1;
			return ST_OK;
		}
		kfree(bk);
		bk = NULL;
	}
	return ST_NOT_FOUND;
}

/* FAT32: does the backup boot sector repeat the main one's parameter block?
 * Only reported (a stale backup is common and harmless for this driver,
 * which never reads it once the main one is good). */
static void fat_check_backup(fat_fs_t *fs, const struct fat_geo *g)
{
	uint8_t *main_bs = NULL, *bk = NULL;
	uint64_t off = (uint64_t)g->backup << fs->fs_shift;

	if (g->fat_bits != 32 || !g->backup)
		return;
	if (fat_read_bytes(fs, 0, 512, &main_bs) != ST_OK)
		return;
	if (fat_read_bytes(fs, off, 512, &bk) == ST_OK) {
		/* The parameter block proper: bytes 11 .. 63 (the state byte
		 * and the boot code may differ). */
		for (unsigned i = FAT_BS_BPS; i < FAT_BS32_DRIVE; i++)
			if (main_bs[i] != bk[i]) {
				kprintf("fat: %s: note: the backup boot sector differs from the boot sector (byte %u)\n",
					fs->bdev->name, i);
				break;
			}
		kfree(bk);
	}
	kfree(main_bs);
}

/* FAT32: read the FSInfo sector.  Its free count and next-free hint are
 * hints only (counted again before they are relied on, unless usefree). */
static void fat_fsinfo_read(fat_fs_t *fs)
{
	uint8_t *buf = NULL;

	fs->fsinfo_valid = 0;
	if (!fs->fsinfo_lba)
		return;
	if (fat_read_bytes(fs, fs->fsinfo_lba << fs->dev_shift, 512, &buf) !=
	    ST_OK)
		return;
	if (fat_le32(buf + FAT_FSI_LEAD_SIG) == FAT_FSI_LEAD_VAL &&
	    fat_le32(buf + FAT_FSI_STRUC_SIG) == FAT_FSI_STRUC_VAL) {
		fs->fsinfo_valid = 1;
		fs->fsinfo_free = fat_le32(buf + FAT_FSI_FREE);
		fs->fsinfo_next = fat_le32(buf + FAT_FSI_NEXT);
	} else {
		kprintf("fat: %s: invalid FSInfo signature; free space is counted\n",
			fs->bdev->name);
	}
	kfree(buf);
}

/* Rewrite the FSInfo sector with the current free count and next-free hint.
 * The sector is read fresh and must still carry its signatures.  Metadata
 * lock exclusive, armed. */
int fat_fsinfo_write(fat_fs_t *fs)
{
	uint8_t *buf = NULL;
	uint32_t fr, nx;
	int r;

	fat_assert_excl(fs);
	if (!fs->fsinfo_lba || !fs->fsinfo_valid || !fs->fsinfo_dirty)
		return ST_OK;
	/* A mount that only freed clusters never counted them: count now,
	 * so that the sector gets the real number instead of "unknown" (a
	 * FAT that cannot be read leaves it unknown -- a hint, never wrong). */
	if (__atomic_load_n(&fs->free_clusters, __ATOMIC_ACQUIRE) < 0)
		(void)fat_count_free(fs);
	r = fat_read_bytes(fs, fs->fsinfo_lba << fs->dev_shift, fs->fs_ss,
			   &buf);
	if (r != ST_OK)
		return r;
	if (fat_le32(buf + FAT_FSI_LEAD_SIG) != FAT_FSI_LEAD_VAL ||
	    fat_le32(buf + FAT_FSI_STRUC_SIG) != FAT_FSI_STRUC_VAL) {
		kfree(buf);
		return fat_corrupt(fs, 0, "FSInfo sector changed under the mount");
	}
	fr = fs->free_clusters >= 0 ? (uint32_t)fs->free_clusters :
				      FAT_FSI_UNKNOWN;
	nx = fs->alloc_cursor >= FAT_FIRST_CLUSTER &&
			     fs->alloc_cursor < fs->cluster_end ?
		     fs->alloc_cursor :
		     FAT_FSI_UNKNOWN;
	if (fat_le32(buf + FAT_FSI_FREE) == fr &&
	    fat_le32(buf + FAT_FSI_NEXT) == nx) {
		kfree(buf);
		fs->fsinfo_dirty = 0;
		return ST_OK;
	}
	fat_put32(buf + FAT_FSI_FREE, fr);
	fat_put32(buf + FAT_FSI_NEXT, nx);
	r = fat_dev_write(fs, fs->fsinfo_lba, fs->boot_dev_sectors, buf,
			  FAT_W_FSINFO);
	kfree(buf);
	if (r == ST_OK)
		fs->fsinfo_dirty = 0;
	return r;
}

/* Set or clear the dirty bit of the boot sector's state byte.  The sector is
 * read fresh (the device holds the truth) and must still describe the
 * volume mounted.  Volumes without an extended boot signature have no state
 * byte (their bytes there are boot code): nothing is written for them.
 * Metadata lock exclusive, armed. */
int fat_boot_write_state(fat_fs_t *fs, int dirty)
{
	uint8_t *buf = NULL;
	uint8_t v;
	int r;

	fat_assert_excl(fs);
	if (!fs->state_off)
		return ST_OK;
	r = fat_read_bytes(fs, 0, fs->fs_ss, &buf);
	if (r != ST_OK)
		return r;
	if (fat_le16(buf + FAT_BS_BPS) != fs->fs_ss ||
	    buf[FAT_BS_MEDIA] != fs->media || buf[FAT_BS_SIGNATURE] != 0x55 ||
	    buf[FAT_BS_SIGNATURE + 1] != 0xAA) {
		kfree(buf);
		return fat_corrupt(fs, 0, "boot sector changed under the mount");
	}
	v = buf[fs->state_off];
	if (dirty)
		v |= FAT_STATE_DIRTY;
	else
		v &= (uint8_t)~FAT_STATE_DIRTY;
	if (v == buf[fs->state_off]) {
		kfree(buf);
		return ST_OK;
	}
	buf[fs->state_off] = v;
	r = fat_dev_write(fs, 0, fs->boot_dev_sectors, buf, FAT_W_BOOT);
	kfree(buf);
	return r;
}

/* ===========================================================================
 * 12. Streams
 *
 * Every file or directory the driver works with is a stream object: what its
 * short entry says (kept authoritative in memory from the moment the object
 * exists -- a size grown by write() is here before it is on the device), its
 * allocation decoded into extents, and its page-cache handle.
 *
 * Two tables find them, both under fs->st_lock:
 *  - by HANDLE (the page cache's chain id carries it): a two-level array,
 *    free slots threaded into a free list;
 *  - by KEY, the location of the short entry: (first cluster of the parent
 *    directory, slot of the short entry).  Directories never change their
 *    first cluster, so the key of a child is stable for as long as its entry
 *    stays where it is; a rename re-keys the stream.  The root has its own
 *    key; children of the FAT12/16 root use 0 as its "first cluster".
 *
 * A stream holds a reference on its parent directory's stream (directories
 * with live children stay cached).  Unreferenced streams sit on an LRU; the
 * oldest clean ones beyond FAT_STREAM_LRU_MAX are forgotten: removed from
 * the tables, their pages and inode-cache entry dropped, THEN their handle
 * freed -- a handle is reused only after every page of its old stream is
 * gone.
 * =========================================================================== */

#define FAT_HCHUNK_SHIFT 12
#define FAT_HCHUNK (1u << FAT_HCHUNK_SHIFT)
#define FAT_HDIR ((uint32_t)((FAT_HANDLE_MAX + 1) >> FAT_HCHUNK_SHIFT))

/* A free slot holds its successor in the free list, tagged with bit 0. */
#define FAT_HSLOT_FREE(next) ((fat_stream_t *)(((uintptr_t)(next) << 1) | 1))
#define FAT_HSLOT_IS_FREE(p) (((uintptr_t)(p)) & 1)
#define FAT_HSLOT_NEXT(p) ((uint32_t)(((uintptr_t)(p)) >> 1))

static inline unsigned fat_khash(uint64_t key)
{
	return (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 54) % FAT_KHASH;
}

/* The stream of handle `h', or NULL.  Not held: valid while the caller
 * holds the metadata lock (streams are destroyed only by puts, and a stream
 * in use holds a reference). */
static fat_stream_t *fat_stream_by_handle(fat_fs_t *fs, uint32_t h)
{
	fat_stream_t *st = NULL;
	uint64_t flags;

	if (h < FAT_HANDLE_FIRST_DYNAMIC || h > FAT_HANDLE_MAX)
		return NULL;
	spin_lock_irqsave(&fs->st_lock, &flags);
	if (fs->htab) {
		fat_stream_t **chunk = fs->htab[h >> FAT_HCHUNK_SHIFT];

		if (chunk) {
			st = chunk[h & (FAT_HCHUNK - 1)];
			if (FAT_HSLOT_IS_FREE(st) || (st && st->dead))
				st = NULL;
		}
	}
	spin_unlock_irqrestore(&fs->st_lock, flags);
	return st;
}

/* Take a handle and point it at `st'.  ST_OK or ST_NOMEM / ST_NOSPC. */
static int fat_handle_alloc(fat_fs_t *fs, fat_stream_t *st)
{
	uint64_t flags;
	uint32_t h;
	fat_stream_t **fresh = NULL;

	for (;;) {
		spin_lock_irqsave(&fs->st_lock, &flags);
		if (fs->hfree_head) {
			h = fs->hfree_head;
		} else if (fs->next_handle <= FAT_HANDLE_MAX) {
			h = fs->next_handle;
		} else {
			spin_unlock_irqrestore(&fs->st_lock, flags);
			if (fresh)
				kfree(fresh);
			return ST_NOSPC;
		}
		fat_stream_t **chunk = fs->htab[h >> FAT_HCHUNK_SHIFT];

		if (!chunk && fresh) {
			fs->htab[h >> FAT_HCHUNK_SHIFT] = fresh;
			chunk = fresh;
			fresh = NULL;
		}
		if (!chunk) {
			spin_unlock_irqrestore(&fs->st_lock, flags);
			fresh = (fat_stream_t **)kcalloc(FAT_HCHUNK,
							 sizeof(*fresh));
			if (!fresh)
				return ST_NOMEM;
			continue;
		}
		if (h == fs->hfree_head) {
			fat_stream_t *slot = chunk[h & (FAT_HCHUNK - 1)];

			fs->hfree_head = FAT_HSLOT_NEXT(slot);
		} else {
			fs->next_handle++;
		}
		chunk[h & (FAT_HCHUNK - 1)] = st;
		st->handle = h;
		spin_unlock_irqrestore(&fs->st_lock, flags);
		if (fresh)
			kfree(fresh);
		return ST_OK;
	}
}

/* Give a handle back.  Its stream's pages are gone already. */
static void fat_handle_free(fat_fs_t *fs, uint32_t h)
{
	uint64_t flags;
	fat_stream_t **chunk;

	if (h < FAT_HANDLE_FIRST_DYNAMIC)
		return;
	spin_lock_irqsave(&fs->st_lock, &flags);
	chunk = fs->htab[h >> FAT_HCHUNK_SHIFT];
	if (chunk) {
		chunk[h & (FAT_HCHUNK - 1)] = FAT_HSLOT_FREE(fs->hfree_head);
		fs->hfree_head = h;
	}
	spin_unlock_irqrestore(&fs->st_lock, flags);
}

static void fat_lru_unlink_locked(fat_fs_t *fs, fat_stream_t *st)
{
	if (!st->on_lru)
		return;
	if (st->lru_prev)
		st->lru_prev->lru_next = st->lru_next;
	else
		fs->lru_head = st->lru_next;
	if (st->lru_next)
		st->lru_next->lru_prev = st->lru_prev;
	else
		fs->lru_tail = st->lru_prev;
	st->lru_prev = st->lru_next = NULL;
	st->on_lru = 0;
	fs->lru_count--;
}

static void fat_lru_push_locked(fat_fs_t *fs, fat_stream_t *st)
{
	st->lru_prev = NULL;
	st->lru_next = fs->lru_head;
	if (fs->lru_head)
		fs->lru_head->lru_prev = st;
	fs->lru_head = st;
	if (!fs->lru_tail)
		fs->lru_tail = st;
	st->on_lru = 1;
	fs->lru_count++;
}

static void fat_khash_unlink_locked(fat_fs_t *fs, fat_stream_t *st)
{
	fat_stream_t **pp;

	if (!st->hashed)
		return;
	pp = &fs->khash[fat_khash(st->key)];
	while (*pp && *pp != st)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = st->hnext;
	st->hnext = NULL;
	st->hashed = 0;
}

static void fat_khash_insert_locked(fat_fs_t *fs, fat_stream_t *st)
{
	unsigned b = fat_khash(st->key);

	st->hnext = fs->khash[b];
	fs->khash[b] = st;
	st->hashed = 1;
}

static fat_stream_t *fat_khash_find_locked(fat_fs_t *fs, uint64_t key)
{
	for (fat_stream_t *s = fs->khash[fat_khash(key)]; s; s = s->hnext)
		if (s->key == key && !s->dead)
			return s;
	return NULL;
}

/* A new reference to a stream found through a table (under st_lock). */
static void fat_stream_hold_locked(fat_fs_t *fs, fat_stream_t *st)
{
	if (st->refs++ == 0)
		fat_lru_unlink_locked(fs, st);
}

void fat_stream_hold(fat_stream_t *st)
{
	uint64_t flags;

	spin_lock_irqsave(&st->fs->st_lock, &flags);
	fat_stream_hold_locked(st->fs, st);
	spin_unlock_irqrestore(&st->fs->st_lock, flags);
}

/* Free a stream that is out of every table. */
static void fat_stream_free(fat_stream_t *st)
{
	if (st->ext)
		kfree(st->ext);
	kfree(st);
}

/* Forget a dead stream: its pages, its inode-cache entry, its handle, and
 * the hold it had on its parent (returned for the caller to put). */
static fat_stream_t *fat_stream_destroy(fat_stream_t *st)
{
	fat_fs_t *fs = st->fs;
	fat_stream_t *parent = st->parent;
	unsigned long chain = fat_chain_of(st);

	/* No page of it is dirty: streams with unstored data are never
	 * evicted, and an orphan's pages are dropped on purpose. */
	pagecache_invalidate_file(&fs->sb, chain);
	icache_remove(&fs->sb, chain);
	if (st->is_dir && (st->first_cluster || st->fixed_root))
		dcache_invalidate_dir(&fs->sb, fat_dkey(st));
	fat_handle_free(fs, st->handle);
	st->parent = NULL;
	fat_stream_free(st);
	return parent;
}

static void fat_stream_put_one(fat_stream_t *st);

/* Evict unreferenced clean streams, oldest first: those beyond the LRU
 * ceiling, and `want' more (reclaim asking for memory).  Returns how many
 * went.  Metadata lock exclusive (no I/O).  A loop: the parent references
 * the evicted ones drop are put without trimming again (no recursion up the
 * directory tree). */
static unsigned long fat_stream_trim(fat_fs_t *fs, unsigned long want)
{
	unsigned long gone = 0;

	fat_assert_excl(fs);
	for (;;) {
		fat_stream_t *victim = NULL, *parent;
		uint64_t flags;

		spin_lock_irqsave(&fs->st_lock, &flags);
		if (fs->lru_count > FAT_STREAM_LRU_MAX || gone < want) {
			for (fat_stream_t *s = fs->lru_tail; s; s = s->lru_prev) {
				if (s->refs == 0 && !s->data_dirty &&
				    !s->meta_dirty && !s->is_root) {
					victim = s;
					break;
				}
			}
		}
		if (victim) {
			fat_lru_unlink_locked(fs, victim);
			fat_khash_unlink_locked(fs, victim);
			victim->dead = 1;
			fs->nstreams--;
		}
		spin_unlock_irqrestore(&fs->st_lock, flags);
		if (!victim)
			return gone;
		gone++;
		parent = fat_stream_destroy(victim);
		if (parent)
			fat_stream_put_one(parent);
	}
}

/* ---- giving memory back ---------------------------------------------- */

static unsigned long fat_shrink_count(void *arg)
{
	return ((fat_fs_t *)arg)->lru_count;
}

/* From the reclaim thread: no lock held, so the metadata lock is taken
 * exclusive, as every other trim runs under it. */
static unsigned long fat_shrink_scan(void *arg, unsigned long nr)
{
	fat_fs_t *fs = arg;
	unsigned long gone;

	fat_meta_wlock(fs);
	gone = fat_stream_trim(fs, nr);
	fat_meta_wunlock(fs);
	return gone;
}

static void fat_shrinker_register(fat_fs_t *fs)
{
	fs->shrinker.name = "fat-streams";
	fs->shrinker.count = fat_shrink_count;
	fs->shrinker.scan = fat_shrink_scan;
	fs->shrinker.arg = fs;
	vm_register_shrinker(&fs->shrinker);
}

/* Drop a reference.  The last one parks the stream on the LRU -- or, for an
 * orphan (unlinked, its clusters handed to the free machinery by the last
 * close), forgets it: at once when this task holds the metadata lock
 * exclusive (then its hold on its parent goes the same way, a loop), else at
 * the end of the next exclusive hold.  No LRU trimming here. */
static void fat_stream_put_one(fat_stream_t *st)
{
	while (st) {
		fat_fs_t *fs = st->fs;
		int excl = fat_rwsem_held_excl(&fs->meta);
		uint64_t flags;
		int destroy = 0;

		spin_lock_irqsave(&fs->st_lock, &flags);
		WARN_ON(st->refs <= 0);
		if (st->refs > 0 && --st->refs == 0) {
			if (st->orphan) {
				fat_khash_unlink_locked(fs, st);
				st->dead = 1;
				fs->nstreams--;
				if (excl) {
					destroy = 1;
				} else {
					st->dnext = fs->reap_head;
					fs->reap_head = st;
				}
			} else {
				fat_lru_push_locked(fs, st);
			}
		}
		spin_unlock_irqrestore(&fs->st_lock, flags);
		st = destroy ? fat_stream_destroy(st) : NULL;
	}
}

/* fat_stream_put_one() and, for a task holding the metadata lock exclusive
 * while the LRU is over its cap, a trim. */
void fat_stream_put(fat_stream_t *st)
{
	fat_fs_t *fs;

	if (!st)
		return;
	fs = st->fs;
	fat_stream_put_one(st);
	if (fs->lru_count > FAT_STREAM_LRU_MAX && fat_rwsem_held_excl(&fs->meta))
		fat_stream_trim(fs, 0);
}

/* The end of an exclusive hold: destroy the parked orphans and trim the
 * LRU.  Metadata lock exclusive. */
void fat_stream_maint(fat_fs_t *fs)
{
	for (;;) {
		fat_stream_t *st;
		uint64_t flags;

		spin_lock_irqsave(&fs->st_lock, &flags);
		st = fs->reap_head;
		if (st)
			fs->reap_head = st->dnext;
		spin_unlock_irqrestore(&fs->st_lock, flags);
		if (!st)
			break;
		st->dnext = NULL;
		fat_stream_put_one(fat_stream_destroy(st));
	}
	if (fs->lru_count > FAT_STREAM_LRU_MAX)
		fat_stream_trim(fs, 0);
}

/* Bytes of the fixed root directory region. */
static inline uint64_t fat_fixed_root_bytes(fat_fs_t *fs)
{
	return (uint64_t)fs->root_entries * FAT_DENTRY_SIZE;
}

/* Fill a stream's allocation from its short entry.  Directories follow
 * their chain to its end (at most FAT_MAX_DIR_BYTES); files need
 * ceil(size / cluster) clusters -- or one, for an empty file that names a
 * cluster anyway (it stays the file's).  `quiet' reports nothing (an unlink
 * of a corrupt file).  Metadata lock held. */
static int fat_stream_decode(fat_fs_t *fs, fat_stream_t *st, int quiet)
{
	uint64_t want;
	int r;

	if (st->fixed_root) {
		st->nclusters = 0;
		st->next = 0;
		st->size = fat_fixed_root_bytes(fs);
		return ST_OK;
	}
	if (st->is_dir) {
		if (!st->first_cluster) {
			if (quiet)
				return ST_IO;
			return fat_corrupt(fs, st->entry_index,
					   "directory without a cluster");
		}
		r = fat_chain_decode(fs, st->first_cluster, 0,
				     FAT_MAX_DIR_BYTES >> fs->cs_shift, quiet,
				     &st->ext, &st->next, &st->ext_cap,
				     &st->nclusters);
		if (r == ST_OK)
			st->size = (uint64_t)st->nclusters << fs->cs_shift;
		return r;
	}
	want = (st->size + fs->cs - 1) >> fs->cs_shift;
	if (want > fs->cluster_count) {
		if (quiet)
			return ST_IO;
		return fat_corrupt(fs, st->first_cluster,
				   "file of %llu bytes larger than the volume",
				   (unsigned long long)st->size);
	}
	if (want == 0) {
		st->nclusters = 0;
		st->next = 0;
		if (!st->first_cluster)
			return ST_OK;
		/* An empty file naming a cluster keeps it (it is reused by the
		 * next write) -- when the cluster is a sane one.  A bogus number
		 * is dropped from memory: nothing is read through it, and the
		 * next store of the entry replaces it. */
		r = fat_chain_decode(fs, st->first_cluster, 1, 0, 1, &st->ext,
				     &st->next, &st->ext_cap, &st->nclusters);
		if (r == ST_NOMEM)
			return r;
		if (r != ST_OK) {
			st->first_cluster = 0;
			st->nclusters = 0;
			st->next = 0;
		}
		return ST_OK;
	}
	if (st->first_cluster == 0) {
		if (quiet)
			return ST_IO;
		return fat_corrupt(fs, st->entry_index,
				   "file of %llu bytes without clusters",
				   (unsigned long long)st->size);
	}
	return fat_chain_decode(fs, st->first_cluster, want, 0, quiet, &st->ext,
				&st->next, &st->ext_cap, &st->nclusters);
}

/* Copy what a short entry says into a stream. */
static void fat_stream_from_di(fat_stream_t *st, const fat_dentry_info_t *di)
{
	mm_memcpy(st->name83, di->name83, 11);
	st->entry_index = di->index;
	st->lfn_count = di->lfn_count;
	st->attr = di->attr;
	st->ntres = di->ntres;
	st->ctime_cs = di->ctime_cs;
	st->ctime = di->ctime;
	st->cdate = di->cdate;
	st->adate = di->adate;
	st->mtime = di->mtime;
	st->mdate = di->mdate;
	st->first_cluster = di->first_cluster;
	st->is_dir = (di->attr & FAT_ATTR_DIR) != 0;
	st->size = st->is_dir ? 0 : di->size;
}

/* The stream of the entry set `di' in directory `parent', held: the existing
 * object when there is one (its in-memory state wins over the entry), else a
 * new one decoded from `di'.  With `quiet', a chain that does not decode
 * gives a stream with no allocation and chain_bad set instead of an error
 * (an unlink of a corrupt file: the name goes, the clusters stay).
 * Metadata lock held. */
fat_stream_t *fat_stream_get_ex(fat_fs_t *fs, fat_stream_t *parent,
				       const fat_dentry_info_t *di, int quiet,
				       int *err)
{
	uint64_t key = FAT_KEY(fat_dir_keycl(parent), di->index);
	fat_stream_t *st, *have;
	uint64_t flags;
	int r;

	fat_assert_meta(fs);
	*err = ST_OK;
	spin_lock_irqsave(&fs->st_lock, &flags);
	st = fat_khash_find_locked(fs, key);
	if (st)
		fat_stream_hold_locked(fs, st);
	spin_unlock_irqrestore(&fs->st_lock, flags);
	if (st)
		return st;

	/* A directory entry naming the clusters of the directory it is in, or
	 * of one above it, would make the tree endless (and give one
	 * directory two streams that cache it separately): corrupt. */
	if (di->attr & FAT_ATTR_DIR) {
		const fat_stream_t *a = parent;

		for (unsigned depth = 0; a && depth < 4096;
		     depth++, a = a->parent)
			if (a->first_cluster == di->first_cluster) {
				*err = fat_corrupt(fs, di->first_cluster,
						   "directory entry (slot %u) names the directory at cluster %u above it",
						   di->index, di->first_cluster);
				return NULL;
			}
	}

	st = (fat_stream_t *)kcalloc(1, sizeof(*st));
	if (!st) {
		*err = ST_NOMEM;
		return NULL;
	}
	st->fs = fs;
	st->key = key;
	fat_stream_from_di(st, di);
	st->subdirs = -1;
	st->dlo = 1;
	st->dhi = 0;
	r = fat_stream_decode(fs, st, quiet);
	if (r != ST_OK) {
		if (st->ext)
			kfree(st->ext);
		st->ext = NULL;
		st->next = 0;
		st->ext_cap = 0;
		st->nclusters = 0;
		if (!quiet || r == ST_NOMEM) {
			kfree(st);
			*err = r;
			return NULL;
		}
		st->chain_bad = 1;
	}
	r = fat_handle_alloc(fs, st);
	if (r != ST_OK) {
		if (st->ext)
			kfree(st->ext);
		kfree(st);
		*err = r;
		return NULL;
	}
	st->refs = 1;

	spin_lock_irqsave(&fs->st_lock, &flags);
	have = fat_khash_find_locked(fs, key);
	if (have) {
		/* Another lookup made it meanwhile: use that one. */
		fat_stream_hold_locked(fs, have);
		spin_unlock_irqrestore(&fs->st_lock, flags);
		fat_handle_free(fs, st->handle);
		fat_stream_free(st);
		return have;
	}
	fat_khash_insert_locked(fs, st);
	fat_stream_hold_locked(fs, parent);
	st->parent = parent;
	fs->nstreams++;
	spin_unlock_irqrestore(&fs->st_lock, flags);
	return st;
}

fat_stream_t *fat_stream_get(fat_fs_t *fs, fat_stream_t *parent,
				    const fat_dentry_info_t *di, int *err)
{
	return fat_stream_get_ex(fs, parent, di, 0, err);
}

/* The existing stream object of a location, held, or NULL. */
fat_stream_t *fat_stream_find(fat_fs_t *fs, uint64_t key)
{
	fat_stream_t *st;
	uint64_t flags;

	spin_lock_irqsave(&fs->st_lock, &flags);
	st = fat_khash_find_locked(fs, key);
	if (st)
		fat_stream_hold_locked(fs, st);
	spin_unlock_irqrestore(&fs->st_lock, flags);
	return st;
}

/* Move a stream to a new location (rename).  Metadata lock exclusive. */
void fat_stream_rekey(fat_fs_t *fs, fat_stream_t *st,
			     fat_stream_t *new_parent, uint32_t index,
			     uint8_t lfn_count)
{
	fat_stream_t *old_parent;
	uint64_t flags;

	fat_assert_excl(fs);
	spin_lock_irqsave(&fs->st_lock, &flags);
	fat_khash_unlink_locked(fs, st);
	st->key = FAT_KEY(fat_dir_keycl(new_parent), index);
	st->entry_index = index;
	st->lfn_count = lfn_count;
	fat_khash_insert_locked(fs, st);
	old_parent = st->parent;
	if (old_parent != new_parent) {
		fat_stream_hold_locked(fs, new_parent);
		st->parent = new_parent;
	} else {
		old_parent = NULL;
	}
	spin_unlock_irqrestore(&fs->st_lock, flags);
	if (old_parent)
		fat_stream_put(old_parent);
}

/* Unlink a stream from its name: it keeps its clusters while referenced
 * and is forgotten at its last put.  Metadata lock exclusive. */
void fat_stream_orphan(fat_fs_t *fs, fat_stream_t *st)
{
	fat_stream_t *parent;
	uint64_t flags;

	fat_assert_excl(fs);
	spin_lock_irqsave(&fs->st_lock, &flags);
	fat_khash_unlink_locked(fs, st);
	st->orphan = 1;
	parent = st->parent;
	st->parent = NULL;
	spin_unlock_irqrestore(&fs->st_lock, flags);
	if (parent)
		fat_stream_put(parent);
}

/* The disk cluster of stream cluster `ci', 0 beyond the allocation. */
static inline uint32_t fat_st_cluster(const fat_stream_t *st, uint64_t ci)
{
	if (ci >= st->nclusters)
		return 0;
	return fat_ext_lookup(st->ext, st->next, ci);
}

/* ===========================================================================
 * 13. Page-cache superblock operations (exfat.h, section 1)
 *
 * Pure memory lookups over the decoded extents (or the fixed root region),
 * under the metadata lock (shared through lock_map, exclusive through
 * lock_io).
 * =========================================================================== */

static inline fat_fs_t *fat_fs_of_sb(vfs_superblock_t *sb)
{
	return (fat_fs_t *)sb->fs_private;
}

/* Exposed blocks a stream maps. */
static uint64_t fat_st_blocks(fat_fs_t *fs, const fat_stream_t *st)
{
	if (st->fixed_root)
		return (fat_fixed_root_bytes(fs) + fs->bs - 1) >> fs->bs_shift;
	return (uint64_t)st->nclusters << fs->bpc_shift;
}

static unsigned long fat_st_block_at(fat_fs_t *fs, const fat_stream_t *st,
				     unsigned long n)
{
	if (n >= fat_st_blocks(fs, st))
		return 0;
	return FAT_BID_ENC(st->handle, n);
}

/* LBA of block n of the stream; 0 without a location. */
unsigned long fat_st_block_lba(fat_fs_t *fs, const fat_stream_t *st,
				      unsigned long n)
{
	uint64_t ci;
	uint32_t disk;

	if (n >= fat_st_blocks(fs, st))
		return 0;
	if (st->fixed_root)
		return (unsigned long)(fs->root_lba +
				       ((uint64_t)n
					<< (fs->bs_shift - fs->dev_shift)));
	ci = (uint64_t)n >> fs->bpc_shift;
	disk = fat_st_cluster(st, ci);
	if (!disk)
		return 0;
	return (unsigned long)(fat_cl_lba(fs, disk) +
			       (((uint64_t)n & ((1ULL << fs->bpc_shift) - 1))
				<< (fs->bs_shift - fs->dev_shift)));
}

static unsigned long fat_sb_block_size(vfs_superblock_t *sb)
{
	return fat_fs_of_sb(sb)->bs;
}

static const block_device_t *fat_sb_bdev(vfs_superblock_t *sb)
{
	return &fat_fs_of_sb(sb)->guard;
}

static unsigned long fat_sb_sector_size(vfs_superblock_t *sb)
{
	return fat_fs_of_sb(sb)->dev_ss;
}

static unsigned long fat_sb_end_of_chain(vfs_superblock_t *sb)
{
	(void)sb;
	return FAT_EOC_MARKER;
}

static unsigned long fat_sb_block_at(vfs_superblock_t *sb, unsigned long chain,
				     unsigned long index)
{
	fat_fs_t *fs = fat_fs_of_sb(sb);
	fat_stream_t *st;

	if (!FAT_BID_IS(chain) || FAT_BID_INDEX(chain) != 0)
		return 0;
	st = fat_stream_by_handle(fs, (uint32_t)FAT_BID_HANDLE(chain));
	if (!st)
		return 0;
	return fat_st_block_at(fs, st, index);
}

static unsigned long fat_sb_next_block(vfs_superblock_t *sb, unsigned long cur)
{
	fat_fs_t *fs = fat_fs_of_sb(sb);
	fat_stream_t *st;
	unsigned long r;

	if (!FAT_BID_IS(cur))
		return FAT_BID_EOC;
	st = fat_stream_by_handle(fs, (uint32_t)FAT_BID_HANDLE(cur));
	if (!st)
		return FAT_BID_EOC;
	r = fat_st_block_at(fs, st, FAT_BID_INDEX(cur) + 1);
	return r ? r : FAT_BID_EOC;
}

static unsigned long fat_sb_block_to_lba(vfs_superblock_t *sb, unsigned long bid)
{
	fat_fs_t *fs = fat_fs_of_sb(sb);
	fat_stream_t *st;

	if (!FAT_BID_IS(bid))
		return 0;
	st = fat_stream_by_handle(fs, (uint32_t)FAT_BID_HANDLE(bid));
	if (!st)
		return 0;
	return fat_st_block_lba(fs, st, FAT_BID_INDEX(bid));
}

static unsigned long fat_sb_map_block(vfs_superblock_t *sb, unsigned long chain,
				      unsigned long index)
{
	unsigned long id = fat_sb_block_at(sb, chain, index);

	if (!id || !fat_sb_block_to_lba(sb, id))
		return 0;
	return id;
}

static void fat_sb_lock_io(vfs_superblock_t *sb)
{
	fat_meta_wlock(fat_fs_of_sb(sb));
}

static void fat_sb_unlock_io(vfs_superblock_t *sb)
{
	fat_rwsem_write_unlock(&fat_fs_of_sb(sb)->meta);
}

static void fat_sb_lock_map(vfs_superblock_t *sb)
{
	fat_meta_rlock(fat_fs_of_sb(sb));
}

static void fat_sb_unlock_map(vfs_superblock_t *sb)
{
	fat_meta_runlock(fat_fs_of_sb(sb));
}

static unsigned long fat_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	(void)sb;
	return 0;
}

/* Generic inode flush (icache_flush): the stream's short entry, if it has
 * changes, goes to the device with a commit. */
static int fat_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *ino)
{
	fat_fs_t *fs = fat_fs_of_sb(sb);
	fat_stream_t *st;
	int r = ST_OK;

	if (!ino || !FAT_BID_IS(ino->start_cluster))
		return 0;
	fat_meta_wlock(fs);
	st = fat_stream_by_handle(fs,
				  (uint32_t)FAT_BID_HANDLE(ino->start_cluster));
	if (st && st->meta_dirty && (fs->armed || fs->may_write))
		r = fat_commit(fs);
	fat_meta_wunlock(fs);
	return r == ST_OK ? 0 : -1;
}

static const vfs_sb_ops_t fat_sb_ops = {
	.block_size = fat_sb_block_size,
	.bdev = fat_sb_bdev,
	.sector_size = fat_sb_sector_size,
	.block_to_lba = fat_sb_block_to_lba,
	.end_of_chain_marker = fat_sb_end_of_chain,
	.next_block = fat_sb_next_block,
	.map_block = fat_sb_map_block,
	.block_at = fat_sb_block_at,
	.write_inode = fat_sb_write_inode,
	.lock_io = fat_sb_lock_io,
	.unlock_io = fat_sb_unlock_io,
	.lock_map = fat_sb_lock_map,
	.unlock_map = fat_sb_unlock_map,
	.reserved_meta_block = fat_sb_reserved_meta_block,
	.alloc_blocks = NULL, /* allocation happens in write() */
};

fat_fs_t *fat_fs_of(vfs_mount_t *m)
{
	return m ? (fat_fs_t *)m->fs_data : NULL;
}

uint64_t fat_ino(uint32_t first, uint32_t dir_keycl, uint32_t index)
{
	if (first)
		return first;
	return FAT_INO_POS(dir_keycl, index);
}

uint64_t fat_st_ino(fat_fs_t *fs, const fat_stream_t *st)
{
	(void)fs;
	if (st->is_root)
		return FAT_ROOT_INO;
	return fat_ino(st->first_cluster,
		       st->parent ? fat_dir_keycl(st->parent) :
				    (uint32_t)(st->key >> 24),
		       st->entry_index);
}

/* Does a short name carry one of the extensions showexec makes
 * executable? */
static int fat_is_exec(const uint8_t *nm)
{
	static const char ext[3][3] = { { 'E', 'X', 'E' },
					{ 'C', 'O', 'M' },
					{ 'B', 'A', 'T' } };

	for (unsigned i = 0; i < 3; i++)
		if (nm[8] == (uint8_t)ext[i][0] && nm[9] == (uint8_t)ext[i][1] &&
		    nm[10] == (uint8_t)ext[i][2])
			return 1;
	return 0;
}

/* The mode bits a file or directory shows: the mount's masks, no write
 * permission for the read-only attribute (directories only with rodir), no
 * execute permission for other than .EXE / .COM / .BAT with showexec. */
unsigned fat_mode_of(fat_fs_t *fs, uint8_t attr, const uint8_t *nm)
{
	unsigned mode;

	if (attr & FAT_ATTR_DIR) {
		mode = S_IFDIR | (0777u & ~fs->opts.dmask);
		if ((attr & FAT_ATTR_RO) && fs->opts.rodir)
			mode &= ~0222u;
	} else {
		mode = S_IFREG | (0777u & ~fs->opts.fmask);
		if (attr & FAT_ATTR_RO)
			mode &= ~0222u;
		if (fs->opts.showexec && nm && !fat_is_exec(nm))
			mode &= ~0111u;
	}
	return mode;
}

/* Count the subdirectories of `d' (st_nlink of a directory), cached. */
static long fat_count_subdirs(fat_fs_t *fs, fat_stream_t *d)
{
	fat_diter_t it;
	uint64_t slots = fat_dir_slots(d);
	long n = 0;

	if (d->subdirs >= 0)
		return d->subdirs;
	fat_diter_init(&it, fs, d);
	for (uint64_t i = 0; i < slots; i++) {
		const uint8_t *e = fat_diter_get(&it, i);

		if (!e || e[0] == FAT_NAME_END)
			break;
		if (fat_de_is_object(e) && (e[FAT_DE_ATTR] & FAT_ATTR_DIR))
			n++;
	}
	fat_diter_done(&it);
	if (n > 65000)
		n = 65000;
	d->subdirs = n;
	return n;
}

/* Times of a stat: modification, access (a date), creation (st_ctime; the
 * modification time when the volume never set one). */
static void fat_stat_times(fat_fs_t *fs, struct kstat *k, uint16_t mtime,
			   uint16_t mdate, uint16_t ctime, uint16_t cdate,
			   uint8_t ccs, uint16_t adate)
{
	fat_time_decode(fs, mtime, mdate, 0, &k->st_mtime, &k->st_mtime_nsec);
	if (adate)
		fat_time_decode(fs, 0, adate, 0, &k->st_atime, &k->st_atime_nsec);
	else
		k->st_atime = k->st_mtime;
	if (cdate)
		fat_time_decode(fs, ctime, cdate, ccs, &k->st_ctime,
				&k->st_ctime_nsec);
	else
		k->st_ctime = k->st_mtime;
}

static unsigned long fat_blksize(fat_fs_t *fs)
{
	return fs->cs > PAGE_SIZE ? fs->cs : PAGE_SIZE;
}

static void fat_stat_from_stream(fat_fs_t *fs, fat_stream_t *st,
				 struct kstat *k)
{
	mm_memset(k, 0, sizeof(*k));
	k->st_ino = fat_st_ino(fs, st);
	k->st_uid = fs->opts.uid;
	k->st_gid = fs->opts.gid;
	k->st_blksize = fat_blksize(fs);
	k->st_blocks = st->fixed_root ?
			       fat_round_up(fat_fixed_root_bytes(fs), 512) >> 9 :
			       ((uint64_t)st->nclusters << fs->cs_shift) >> 9;
	k->st_mode = fat_mode_of(fs, st->is_root ? FAT_ATTR_DIR : st->attr,
				 st->is_root ? NULL : st->name83);
	if (st->is_dir) {
		k->st_nlink = (uint32_t)(2 + fat_count_subdirs(fs, st));
		k->st_size = st->size;
	} else {
		k->st_nlink = st->orphan ? 0 : 1;
		k->st_size = st->size;
	}
	if (st->is_root)
		return; /* the root has no entry: no times of its own */
	fat_stat_times(fs, k, st->mtime, st->mdate, st->ctime, st->cdate,
		       st->ctime_cs, st->adate);
}

static void fat_stat_from_dentry(fat_fs_t *fs, fat_stream_t *dir,
				 const fat_dentry_info_t *di, struct kstat *k)
{
	mm_memset(k, 0, sizeof(*k));
	k->st_ino = fat_ino(di->first_cluster, fat_dir_keycl(dir), di->index);
	k->st_uid = fs->opts.uid;
	k->st_gid = fs->opts.gid;
	k->st_blksize = fat_blksize(fs);
	k->st_blocks = fat_round_up(di->size, fs->cs) >> 9;
	k->st_mode = fat_mode_of(fs, di->attr, di->name83);
	k->st_nlink = 1;
	k->st_size = di->size;
	fat_stat_times(fs, k, di->mtime, di->mdate, di->ctime, di->cdate,
		       di->ctime_cs, di->adate);
}

/* stat of a path.  Files are described from their entry (or their stream
 * object when one exists: it is newer); directories through their stream
 * (st_nlink needs the subdirectory count).  Metadata lock shared. */
int fat_stat_locked(fat_fs_t *fs, fat_ctx_t *ctx, const char *path,
			   struct kstat *k)
{
	struct fat_walk w;
	int r = fat_walk(fs, ctx, path, &w);

	if (r != ST_OK)
		return r;
	if (w.is_root || w.st) {
		fat_stat_from_stream(fs, w.st, k);
		fat_walk_release(&w);
		return ST_OK;
	}
	if (!w.found) {
		fat_walk_release(&w);
		return ST_NOT_FOUND;
	}
	{
		fat_stream_t *st = fat_stream_find(
			fs, FAT_KEY(fat_dir_keycl(w.dir), w.di.index));

		if (!st && (w.di.attr & FAT_ATTR_DIR)) {
			int err;

			st = fat_stream_get(fs, w.dir, &w.di, &err);
			if (!st) {
				fat_walk_release(&w);
				return err;
			}
		}
		if (st) {
			fat_stat_from_stream(fs, st, k);
			fat_stream_put(st);
		} else {
			fat_stat_from_dentry(fs, w.dir, &w.di, k);
		}
	}
	fat_walk_release(&w);
	return ST_OK;
}

static int fat_stat_op(vfs_mount_t *m, const char *path, struct kstat *k)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	int r;

	if (!fs || !path || !k)
		return ST_INVALID;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_NOMEM;
	fat_meta_rlock(fs);
	r = fat_stat_locked(fs, ctx, path, k);
	fat_meta_runlock(fs);
	fat_ctx_free(ctx);
	return r;
}

static int fat_fstat_op(vfs_file_t *f, struct kstat *k)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	fat_fs_t *fs;

	if (!h || !k)
		return ST_INVALID;
	fs = h->fs;
	fat_meta_rlock(fs);
	fat_stat_from_stream(fs, h->st, k);
	fat_meta_runlock(fs);
	return ST_OK;
}

/* ---- whole-filesystem operations ------------------------------------------ */

static int fat_statfs_op(vfs_mount_t *m, struct vfs_statfs *out)
{
	fat_fs_t *fs = fat_fs_of(m);
	int64_t fr;

	if (!fs || !out)
		return ST_INVALID;
	fat_meta_rlock(fs);
	fat_count_free(fs);
	fr = __atomic_load_n(&fs->free_clusters, __ATOMIC_ACQUIRE);
	fat_meta_runlock(fs);
	if (fr < 0)
		fr = 0;
	mm_memset(out, 0, sizeof(*out));
	out->f_type = FAT_STATFS_MAGIC;
	out->f_bsize = fs->cs;
	out->f_frsize = fs->cs;
	out->f_blocks = fs->cluster_count;
	out->f_bfree = (unsigned long)fr;
	out->f_bavail = (unsigned long)fr;
	out->f_files = 0;
	out->f_ffree = 0;
	out->f_fsid = fs->serial;
	out->f_namelen = FAT_NAME_MAX;
	return ST_OK;
}

/* fsync: the file's data, every pending entry, a device sync.  Any
 * write-back error of the file since the last report is returned (and
 * reported once). */
static int fat_fsync_op(vfs_file_t *f)
{
	fat_file_t *h = f ? (fat_file_t *)f->fs_private : NULL;
	fat_stream_t *st;
	fat_fs_t *fs;
	int r = ST_OK;

	if (!h)
		return ST_INVALID;
	fs = h->fs;
	st = h->st;
	fat_meta_wlock(fs);
	if ((fs->armed || fs->may_write) && !fs->ro_latched && !fs->ro) {
		if (st->data_dirty) {
			uint64_t e0 = fs->io_errors;

			pagecache_flush_file(&fs->sb, fat_chain_of(st));
			if (fs->io_errors != e0)
				st->wb_err = ST_IO;
		}
		r = fat_commit(fs);
		if (r == ST_OK && fs->armed)
			r = fat_dev_sync(fs);
	} else if (fs->ro_latched && (h->wrote || st->data_dirty)) {
		r = ST_IO;
	}
	if (st->wb_err) {
		if (r == ST_OK)
			r = st->wb_err;
		st->wb_err = 0;
	}
	if (r == ST_OK && h->werr) {
		r = h->werr;
		h->werr = 0;
	}
	fat_meta_wunlock(fs);
	return r;
}

static int fat_sync_op(vfs_mount_t *m)
{
	fat_fs_t *fs = fat_fs_of(m);
	int r;

	if (!fs)
		return ST_INVALID;
	fat_meta_wlock(fs);
	r = fat_clean(fs);
	fat_meta_wunlock(fs);
	return r;
}

/* Access decisions: without ownership options every user may do what the
 * reported mode bits suggest for the owner (the volume stores no owners)
 * -- except writing a read-only file, which the generic check of the mode
 * bits then decides (only the privileged pass).  With any ownership option
 * the generic check of uid/gid/mode decides. */
static int fat_permission_op(vfs_mount_t *m, const char *path,
			     unsigned long ino, int want)
{
	fat_fs_t *fs = fat_fs_of(m);
	fat_ctx_t *ctx;
	struct kstat k;
	int r;

	(void)ino;
	if (!fs)
		return ST_UNSUPPORTED;
	if (fs->opts.perm_opts)
		return ST_UNSUPPORTED;
	if (!(want & MAY_WRITE) || !path)
		return ST_OK;
	ctx = fat_ctx_new();
	if (!ctx)
		return ST_OK;
	fat_meta_rlock(fs);
	r = fat_stat_locked(fs, ctx, path, &k);
	fat_meta_runlock(fs);
	fat_ctx_free(ctx);
	if (r == ST_OK && !(k.st_mode & 0222))
		return ST_UNSUPPORTED;
	return ST_OK;
}

static int fat_release_locks_op(vfs_mount_t *m, uint64_t task_id)
{
	fat_fs_t *fs = fat_fs_of(m);
	int released = 0;

	if (!fs)
		return 0;
	released |= fat_rwsem_release_owner(&fs->alloc, task_id);
	released |= fat_rwsem_release_owner(&fs->meta, task_id);
	for (int i = 0; i < FAT_SLOCKS; i++)
		released |= fat_rwsem_release_owner(&fs->slock[i], task_id);
	if (fs->bdev && fs->bdev->release_locks_for_task)
		released |= fs->bdev->release_locks_for_task(fs->bdev, task_id);
	return released;
}

const vfs_ops_t fat_vfs_ops = {
	.open = fat_open_op,
	.open_mode = fat_open_mode_op,
	.stat = fat_stat_op,
	.lstat = fat_stat_op,
	.fstat = fat_fstat_op,
	.read = fat_read_op,
	.read_at = fat_read_at_op,
	.write = fat_write_op,
	.write_at = fat_write_at_op,
	.seek = fat_seek_op,
	.readdir = fat_readdir_op,
	.truncate = fat_truncate_op,
	.unlink = fat_unlink_op,
	.rename = fat_rename_op,
	.mkdir = fat_mkdir_op,
	.rmdir = fat_rmdir_op,
	.chdir = fat_chdir_op,
	.close = fat_close_op,
	.release_locks_for_task = fat_release_locks_op,
	.fsync = fat_fsync_op,
	.chmod = fat_chmod_op,
	.fchmod = fat_fchmod_op,
	.chown = fat_chown_op,
	.fchown = fat_fchown_op,
	.utimens = fat_utimens_op,
	.futimens = fat_futimens_op,
	.statfs = fat_statfs_op,
	.sync = fat_sync_op,
	.getxattr = fat_getxattr_op,
	.setxattr = fat_setxattr_op,
	.listxattr = fat_listxattr_op,
	.removexattr = fat_removexattr_op,
	.fgetxattr = fat_fgetxattr_op,
	.fsetxattr = fat_fsetxattr_op,
	.flistxattr = fat_flistxattr_op,
	.fremovexattr = fat_fremovexattr_op,
	.permission = fat_permission_op,
	/* No symbolic links, hard links or device nodes on FAT: those
	 * operations stay NULL and the VFS answers for them. */
};

/* ===========================================================================
 * 21. Mount options (fat32.h, section 5)
 * =========================================================================== */

static int fat_opt_num(const char *v, unsigned base, uint32_t *out)
{
	uint64_t n = 0;

	if (!v || !*v)
		return 0;
	for (; *v; v++) {
		unsigned d;

		if (*v >= '0' && *v <= '9')
			d = (unsigned)(*v - '0');
		else
			return 0;
		if (d >= base)
			return 0;
		n = n * base + d;
		if (n > 0xFFFFFFFFULL)
			return 0;
	}
	*out = (uint32_t)n;
	return 1;
}

static int fat_opt_int(const char *v, int *out)
{
	int neg = 0;
	uint32_t n;

	if (!v)
		return 0;
	if (*v == '-' || *v == '+') {
		neg = *v == '-';
		v++;
	}
	if (!fat_opt_num(v, 10, &n) || n > 100000)
		return 0;
	*out = neg ? -(int)n : (int)n;
	return 1;
}

/* A yes/no value: none, 1/yes/true, 0/no/false.  -1 for anything else. */
static int fat_opt_bool(const char *v)
{
	if (!v || fat_streq_ci(v, "1") || fat_streq_ci(v, "yes") ||
	    fat_streq_ci(v, "true"))
		return 1;
	if (fat_streq_ci(v, "0") || fat_streq_ci(v, "no") ||
	    fat_streq_ci(v, "false"))
		return 0;
	return -1;
}

static const struct {
	const char *name;
	int mode;
} fat_shortname_modes[] = {
	{ "lower", FAT_SN_LOWER },
	{ "win95", FAT_SN_WIN95 },
	{ "winnt", FAT_SN_WINNT },
	{ "mixed", FAT_SN_MIXED },
};

static const char *fat_shortname_name(int mode)
{
	for (unsigned i = 0;
	     i < sizeof(fat_shortname_modes) / sizeof(fat_shortname_modes[0]);
	     i++)
		if (fat_shortname_modes[i].mode == mode)
			return fat_shortname_modes[i].name;
	return "mixed";
}

/* Parse `data' over the values in `o' (comma separated, vfs_next_option).
 * Unknown options -- the generic flags among them -- are accepted and have
 * no effect.  ST_INVALID for a value that is not one; `o' is then
 * unchanged. */
static int fat_parse_options(const char *data, struct fat_opts *o)
{
	struct fat_opts n = *o;
	const char *cur = data;
	char tmp[VFS_MOUNT_OPTS_MAX];
	const char *key, *val;
	uint32_t umask = 0, v;
	int have_umask = 0, have_fmask = 0, have_dmask = 0, iv, b;

	while (data && vfs_next_option(&cur, tmp, sizeof(tmp), &key, &val)) {
		if (fat_streq(key, "uid")) {
			if (!fat_opt_num(val, 10, &v))
				return ST_INVALID;
			n.uid = v;
			n.perm_opts = 1;
		} else if (fat_streq(key, "gid")) {
			if (!fat_opt_num(val, 10, &v))
				return ST_INVALID;
			n.gid = v;
			n.perm_opts = 1;
		} else if (fat_streq(key, "umask")) {
			if (!fat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			umask = v;
			have_umask = 1;
			n.perm_opts = 1;
		} else if (fat_streq(key, "fmask")) {
			if (!fat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.fmask = v;
			have_fmask = 1;
			n.perm_opts = 1;
		} else if (fat_streq(key, "dmask")) {
			if (!fat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.dmask = v;
			have_dmask = 1;
			n.perm_opts = 1;
		} else if (fat_streq(key, "allow_utime")) {
			if (!fat_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.allow_utime = v;
		} else if (fat_streq(key, "codepage")) {
			if (!fat_opt_num(val, 10, &v) || !fat_cp_find((int)v))
				return ST_INVALID;
			n.codepage = (int)v;
		} else if (fat_streq(key, "iocharset")) {
			if (!val)
				return ST_INVALID;
			if (fat_streq_ci(val, "utf8") || fat_streq_ci(val, "utf-8"))
				n.iocharset = FAT_IOCS_UTF8;
			else if (fat_streq_ci(val, "iso8859-1") ||
				 fat_streq_ci(val, "latin1"))
				n.iocharset = FAT_IOCS_LATIN1;
			else if (fat_streq_ci(val, "ascii"))
				n.iocharset = FAT_IOCS_ASCII;
			else
				return ST_INVALID;
		} else if (fat_streq(key, "utf8")) {
			b = fat_opt_bool(val);
			if (b < 0)
				return ST_INVALID;
			if (b)
				n.iocharset = FAT_IOCS_UTF8;
			else if (n.iocharset == FAT_IOCS_UTF8)
				n.iocharset = FAT_IOCS_LATIN1;
		} else if (fat_streq(key, "uni_xlate")) {
			b = fat_opt_bool(val);
			if (b < 0)
				return ST_INVALID;
			n.uni_xlate = b;
		} else if (fat_streq(key, "shortname")) {
			int found = -1;

			for (unsigned i = 0; val &&
					     i < sizeof(fat_shortname_modes) /
							 sizeof(fat_shortname_modes[0]);
			     i++)
				if (fat_streq_ci(val, fat_shortname_modes[i].name))
					found = fat_shortname_modes[i].mode;
			if (found < 0)
				return ST_INVALID;
			n.shortname = found;
		} else if (fat_streq(key, "nonumtail")) {
			b = fat_opt_bool(val);
			if (b < 0)
				return ST_INVALID;
			n.nonumtail = b;
		} else if (fat_streq(key, "numtail")) {
			n.nonumtail = 0;
		} else if (fat_streq(key, "errors")) {
			if (!val)
				return ST_INVALID;
			if (fat_streq(val, "remount-ro"))
				n.errors = FAT_ERRORS_RO;
			else if (fat_streq(val, "continue"))
				n.errors = FAT_ERRORS_CONTINUE;
			else if (fat_streq(val, "panic"))
				n.errors = FAT_ERRORS_PANIC;
			else
				return ST_INVALID;
		} else if (fat_streq(key, "discard")) {
			n.discard = 1;
		} else if (fat_streq(key, "nodiscard")) {
			n.discard = 0;
		} else if (fat_streq(key, "flush")) {
			n.flush = 1;
		} else if (fat_streq(key, "quiet")) {
			n.quiet = 1;
		} else if (fat_streq(key, "showexec")) {
			n.showexec = 1;
		} else if (fat_streq(key, "rodir")) {
			n.rodir = 1;
		} else if (fat_streq(key, "sys_immutable")) {
			n.sys_immutable = 1;
		} else if (fat_streq(key, "usefree")) {
			n.usefree = 1;
		} else if (fat_streq(key, "dos1xfloppy")) {
			n.dos1xfloppy = 1;
		} else if (fat_streq(key, "tz")) {
			if (!val || !fat_streq_ci(val, "UTC"))
				return ST_INVALID;
			n.tz_utc = 1;
			n.time_offset_given = 0;
		} else if (fat_streq(key, "time_offset")) {
			if (!fat_opt_int(val, &iv) || iv < -24 * 60 ||
			    iv > 24 * 60)
				return ST_INVALID;
			n.time_offset = iv;
			n.time_offset_given = 1;
			n.tz_utc = 0;
		} else if (fat_streq(key, "fat")) {
			if (!fat_opt_num(val, 10, &v) ||
			    (v != 12 && v != 16 && v != 32))
				return ST_INVALID;
			n.fat_bits = (int)v;
		}
		/* nfs=, check=, debug, conv=, the generic flags: accepted. */
	}
	if (have_umask) {
		if (!have_fmask)
			n.fmask = umask;
		if (!have_dmask)
			n.dmask = umask;
	}
	*o = n;
	return ST_OK;
}

static void fat_opts_default(struct fat_opts *o)
{
	mm_memset(o, 0, sizeof(*o));
	o->fmask = FAT_DEFAULT_FMASK;
	o->dmask = FAT_DEFAULT_DMASK;
	o->allow_utime = 0xFFFFFFFFu;
	o->errors = FAT_ERRORS_RO;
	o->shortname = FAT_SN_MIXED;
	o->codepage = FAT_DEFAULT_CODEPAGE;
	o->iocharset = FAT_IOCS_UTF8;
}

/* "uid=..,gid=..,fmask=..,dmask=..,codepage=..,iocharset=..,shortname=..,
 * errors=.." plus what is set of the others. */
static long fat_fst_show_options(vfs_mount_t *m, char *buf, long cap)
{
	fat_fs_t *fs = fat_fs_of(m);
	static const char *const errs[] = { "remount-ro", "continue", "panic" };
	static const char *const iocs[] = { "utf8", "iso8859-1", "ascii" };
	long n = 0;
	int k;

	if (!fs || !buf || cap < 1)
		return 0;
#define FAT_SHOW(...)                                                         \
	do {                                                                  \
		if (n < cap) {                                                \
			k = ksnprintf(buf + n, (size_t)(cap - n), __VA_ARGS__); \
			if (k > 0)                                            \
				n += k;                                       \
		}                                                             \
	} while (0)
	FAT_SHOW("uid=%u,gid=%u,fmask=%04o,dmask=%04o", (unsigned)fs->opts.uid,
		 (unsigned)fs->opts.gid, (unsigned)fs->opts.fmask,
		 (unsigned)fs->opts.dmask);
	if (fs->opts.allow_utime != 0xFFFFFFFFu)
		FAT_SHOW(",allow_utime=%04o", (unsigned)fs->opts.allow_utime);
	FAT_SHOW(",codepage=%d,iocharset=%s,shortname=%s,errors=%s",
		 fs->opts.codepage,
		 iocs[fs->opts.iocharset >= 0 && fs->opts.iocharset <= 2 ?
			      fs->opts.iocharset :
			      0],
		 fat_shortname_name(fs->opts.shortname),
		 errs[fs->opts.errors >= 0 && fs->opts.errors <= 2 ?
			      fs->opts.errors :
			      0]);
	if (fs->opts.uni_xlate)
		FAT_SHOW(",uni_xlate");
	if (fs->opts.nonumtail)
		FAT_SHOW(",nonumtail");
	if (fs->opts.quiet)
		FAT_SHOW(",quiet");
	if (fs->opts.showexec)
		FAT_SHOW(",showexec");
	if (fs->opts.rodir)
		FAT_SHOW(",rodir");
	if (fs->opts.sys_immutable)
		FAT_SHOW(",sys_immutable");
	if (fs->opts.flush)
		FAT_SHOW(",flush");
	if (fs->opts.usefree)
		FAT_SHOW(",usefree");
	if (fs->opts.discard)
		FAT_SHOW(",discard");
	if (fs->opts.dos1xfloppy)
		FAT_SHOW(",dos1xfloppy");
	if (fs->opts.tz_utc)
		FAT_SHOW(",tz=UTC");
	else if (fs->opts.time_offset_given)
		FAT_SHOW(",time_offset=%d", fs->opts.time_offset);
	if (fs->opts.fat_bits)
		FAT_SHOW(",fat=%d", fs->opts.fat_bits);
#undef FAT_SHOW
	if (n >= cap)
		n = cap - 1;
	buf[n] = 0;
	return n;
}

/* ===========================================================================
 * 22. Mount, unmount, remount, probe; the root filesystem
 * =========================================================================== */

/* Does `bdev' hold a FAT volume at its start?  One bounded read of the
 * first sector; a parameter block that parses (quietly), a signature or a
 * jump instruction, and no other file system's name. */
static int fat_probe_boot(struct block_device *bdev)
{
	const block_device_t *b = bdev;
	unsigned long ss;
	struct fat_geo g;
	uint8_t *buf;
	int ok = 0;

	might_sleep();
	if (!b || !b->read)
		return 0;
	ss = b->sector_size ? b->sector_size : 512;
	if (ss < 512 || ss > 4096 || (ss & (ss - 1)) || b->total_sectors < 2)
		return 0;
	buf = (uint8_t *)kalloc(ss);
	if (!buf)
		return 0;
	if (b->read((block_device_t *)b, 0, 1, buf) == ST_OK &&
	    ((buf[FAT_BS_SIGNATURE] == 0x55 &&
	      buf[FAT_BS_SIGNATURE + 1] == 0xAA) ||
	     fat_has_jump(buf)) &&
	    fat_bpb_parse(buf, &g, b->name, 0, 1) == ST_OK && g.bps >= ss)
		ok = 1;
	kfree(buf);
	return ok;
}

/* Free every stream of an instance (unmount, failed mount).  Nothing is
 * open; the caches were released. */
static void fat_streams_destroy(fat_fs_t *fs)
{
	while (fs->reap_head) {
		fat_stream_t *st = fs->reap_head;

		fs->reap_head = st->dnext;
		fat_stream_free(st);
	}
	for (unsigned b = 0; b < FAT_KHASH; b++) {
		fat_stream_t *st = fs->khash[b];

		while (st) {
			fat_stream_t *n = st->hnext;

			fat_stream_free(st);
			st = n;
		}
		fs->khash[b] = NULL;
	}
	fs->root = NULL;
	fs->lru_head = fs->lru_tail = NULL;
	fs->lru_count = 0;
	fs->dirty_head = NULL;
	if (fs->htab) {
		for (uint32_t i = 0; i < FAT_HDIR; i++)
			if (fs->htab[i])
				kfree(fs->htab[i]);
		kfree(fs->htab);
		fs->htab = NULL;
	}
}

static void fat_fs_free(fat_fs_t *fs)
{
	/* First: waits for a scan of this filesystem in progress. */
	vm_unregister_shrinker(&fs->shrinker);
	fat_streams_destroy(fs);
	fat_mc_destroy(fs);
	if (fs->pf)
		kfree(fs->pf);
	if (fs->dfat)
		kfree(fs->dfat);
	if (fs->lnk)
		kfree(fs->lnk);
	kfree(fs);
}

/* Record a reason the mount is read-only.  Every reason is kept (a bit
 * each): lifting one -- a read-write remount lifts the requested one --
 * leaves the mount read-only while any other holds. */
static void fat_gate(fat_fs_t *fs, int reason)
{
	fs->gates |= 1u << reason;
	fs->ro = 1;
}

/* The reasons in `mask' as one line ("a; b"). */
static void fat_gates_text(unsigned mask, char *buf, unsigned long cap)
{
	unsigned long n = 0;

	buf[0] = 0;
	for (int i = 1; i < FAT_RO_NREASONS; i++) {
		int k;

		if (!(mask & (1u << i)) || n + 3 >= cap)
			continue;
		k = ksnprintf(buf + n, (size_t)(cap - n), "%s%s", n ? "; " : "",
			      fat_ro_reason_text[i]);
		if (k > 0)
			n += (unsigned long)k;
		if (n >= cap)
			n = cap - 1;
	}
}

/* Bring up an instance of the volume on `b': geometry, gates, the FAT's
 * first entries, the root directory stream.  Reads only, through no cache
 * but the driver's own (the page cache may not exist yet when the root
 * filesystem is mounted).  `m' may be NULL (the root).  *out on success. */
static int fat_instance_up(block_device_t *b, vfs_mount_t *m, int rdonly,
			   const struct fat_opts *o, int quiet, fat_fs_t **out)
{
	struct fat_geo g;
	fat_fs_t *fs;
	int from_backup = 0, r;
	unsigned long ss;
	uint64_t fat_start, fat_end, vol_bytes;

	*out = NULL;
	ss = b->sector_size ? b->sector_size : 512;
	if (ss < 512 || ss > 4096 || !fat_is_pow2(ss) || !b->read)
		return ST_INVALID;

	fs = (fat_fs_t *)kcalloc(1, sizeof(*fs));
	if (!fs)
		return ST_NOMEM;
	fs->bdev = b;
	fs->mnt = m;
	fs->dev_ss = (unsigned)ss;
	fs->dev_shift = fat_ilog2(ss);
	fs->opts = *o;
	fs->free_clusters = -1;
	fs->alloc_cursor = FAT_FIRST_CLUSTER;
	fs->next_handle = FAT_HANDLE_FIRST_DYNAMIC;
	fat_rwsem_init(&fs->meta, "fat_meta");
	fat_rwsem_init(&fs->alloc, "fat_alloc");
	for (int i = 0; i < FAT_SLOCKS; i++)
		fat_rwsem_init(&fs->slock[i], "fat_stream");
	spinlock_init(&fs->st_lock, "fat_streams");
	spinlock_init(&fs->mc_lock, "fat_mcache");
	fat_guard_init(fs);
	fs->sb.ops = &fat_sb_ops;
	fs->sb.fs_private = fs;
	fs->cp = fat_cp_find(o->codepage);
	if (!fs->cp)
		fs->cp = fat_cp_find(FAT_DEFAULT_CODEPAGE);

	r = fat_boot_load(fs, &g, &from_backup, quiet);
	if (r != ST_OK) {
		kfree(fs);
		return r == ST_NOMEM ? ST_NOMEM : ST_NOT_FOUND;
	}
	if (g.bps < fs->dev_ss) {
		kprintf("fat: %s: logical sector size %u smaller than the device's %lu\n",
			b->name, g.bps, ss);
		kfree(fs);
		return ST_INVALID;
	}
	fs->fs_ss = g.bps;
	fs->fs_shift = fat_ilog2(g.bps);
	fs->cs_shift = fs->fs_shift + fat_ilog2(g.spc);
	fs->cs = 1UL << fs->cs_shift;
	fs->bs_shift = fs->cs_shift < 12 ? fs->cs_shift : 12;
	fs->bs = 1UL << fs->bs_shift;
	fs->bpc_shift = fs->cs_shift - fs->bs_shift;
	fs->fat_bits = g.fat_bits;
	fs->cluster_count = (uint32_t)g.cluster_count;
	fs->cluster_end = fs->cluster_count + FAT_FIRST_CLUSTER;
	fs->root_cluster = g.fat_bits == 32 ? g.root_cluster : 0;
	fs->nfats = g.nfats;
	fs->media = (uint8_t)g.media;
	fs->serial = g.serial;
	mm_memcpy(fs->label, g.label, 11);
	fs->state_off = g.has_ext ? g.state_off : 0;
	fs->state_byte = g.state;
	fs->boot_dev_sectors = g.bps >> fs->dev_shift;
	if (g.fat_bits == 32 && (g.extflags & FAT_EXTFLAGS_NOMIRROR)) {
		fs->mirror = 0;
		fs->active_fat = g.extflags & FAT_EXTFLAGS_ACTIVE;
	} else {
		fs->mirror = 1;
		fs->active_fat = 0;
	}
	switch (g.fat_bits) {
	case 12:
		fs->eoc_min = FAT12_EOC_MIN;
		fs->eoc_val = FAT12_EOC;
		fs->bad_val = FAT12_BAD;
		fs->ent_mask = 0xFFF;
		break;
	case 16:
		fs->eoc_min = FAT16_EOC_MIN;
		fs->eoc_val = FAT16_EOC;
		fs->bad_val = FAT16_BAD;
		fs->ent_mask = 0xFFFF;
		break;
	default:
		fs->eoc_min = FAT32_EOC_MIN;
		fs->eoc_val = FAT32_EOC;
		fs->bad_val = FAT32_BAD;
		fs->ent_mask = FAT32_MASK;
		break;
	}
	/* Regions, in device sectors. */
	fat_start = (uint64_t)g.reserved << fs->fs_shift;
	fs->fat_bytes = g.fat_length << fs->fs_shift;
	for (unsigned i = 0; i < g.nfats; i++)
		fs->fat_lba[i] = (fat_start + (uint64_t)i * fs->fat_bytes) >>
				 fs->dev_shift;
	fat_end = fat_start + (uint64_t)g.nfats * fs->fat_bytes;
	fs->fat_used_bytes = fat_ent_bytes(g.fat_bits, fs->cluster_end);
	if (g.root_dir_sectors) {
		fs->root_lba = fat_end >> fs->dev_shift;
		fs->root_entries = g.root_entries;
		fs->root_dev_sectors = (g.root_dir_sectors << fs->fs_shift) >>
				       fs->dev_shift;
	}
	fs->data_lba = (g.data_start << fs->fs_shift) >> fs->dev_shift;
	fs->data_dev_sectors = (uint64_t)fs->cluster_count
			       << (fs->cs_shift - fs->dev_shift);
	vol_bytes = g.total_sectors << fs->fs_shift;
	fs->vol_dev_sectors = fat_min64(vol_bytes >> fs->dev_shift,
					b->total_sectors);
	if (g.fat_bits == 32 && g.fsinfo)
		fs->fsinfo_lba = ((uint64_t)g.fsinfo << fs->fs_shift) >>
				 fs->dev_shift;
	/* FAT units: 4 KB (or the device sector, if larger); the whole FAT
	 * for FAT12, whose entries straddle bytes. */
	if (g.fat_bits == 12) {
		fs->mcu_bytes = fat_round_up(fs->fat_used_bytes, fs->dev_ss);
		if (fs->mcu_bytes > fs->fat_bytes)
			fs->mcu_bytes = fs->fat_bytes;
	} else {
		fs->mcu_bytes = fs->dev_ss > 4096 ? fs->dev_ss : 4096;
	}
	if ((fs->mcu_bytes >> fs->dev_shift) > FAT_MC_MAX_SECTORS ||
	    fs->fat_used_bytes > fs->fat_bytes) {
		kprintf("fat: %s: FAT geometry not supported\n", b->name);
		kfree(fs);
		return ST_INVALID;
	}

	/* Read-only gates: one log line, the mount still works. */
	if (rdonly)
		fat_gate(fs, FAT_RO_REQUESTED);
	if (blk_bdev_read_only(b))
		fat_gate(fs, FAT_RO_DEVICE);
	if (from_backup)
		fat_gate(fs, FAT_RO_BACKUP_BOOT);
	if (g.fat_bits == 32 && g.version != 0)
		fat_gate(fs, FAT_RO_VERSION);
	if (vol_bytes > (uint64_t)b->total_sectors << fs->dev_shift)
		fat_gate(fs, FAT_RO_GEOMETRY);

	fs->htab = (fat_stream_t ***)kcalloc(FAT_HDIR, sizeof(*fs->htab));
	if (!fs->htab) {
		fat_fs_free(fs);
		return ST_NOMEM;
	}

	fat_meta_wlock(fs);
	/* FAT[0] carries the media byte, FAT[1] the clean / error flags. */
	{
		struct fat_fatcur cur = { NULL };
		uint32_t f0 = 0, f1 = 0;

		r = fat_ent_get(fs, &cur, 0, &f0);
		if (r == ST_OK)
			r = fat_ent_get(fs, &cur, 1, &f1);
		fat_fatcur_done(fs, &cur);
		if (r != ST_OK) {
			kprintf("fat: %s: FAT unreadable (%d)\n", b->name, r);
			r = ST_IO;
			goto fail_locked;
		}
		if ((f0 & 0xFF) != fs->media) {
			if (!fat_valid_media(f0 & 0xFF))
				fat_gate(fs, FAT_RO_FAT_MEDIA);
			else
				kprintf("fat: %s: note: FAT media byte 0x%02x differs from the boot sector's 0x%02x\n",
					b->name, f0 & 0xFF, fs->media);
		}
		if (fs->state_off && (fs->state_byte & FAT_STATE_DIRTY))
			fs->was_dirty = 1;
		if ((g.fat_bits == 16 && !(f1 & FAT16_CLEAN_BIT)) ||
		    (g.fat_bits == 32 && !(f1 & FAT32_CLEAN_BIT)))
			fs->was_dirty = 1;
	}
	/* The root directory stream. */
	{
		fat_stream_t *root = (fat_stream_t *)kcalloc(1, sizeof(*root));

		if (!root) {
			r = ST_NOMEM;
			goto fail_locked;
		}
		root->fs = fs;
		root->is_root = 1;
		root->is_dir = 1;
		root->fixed_root = g.fat_bits != 32;
		root->attr = FAT_ATTR_DIR;
		root->key = FAT_KEY_ROOT;
		root->first_cluster = fs->root_cluster;
		root->subdirs = -1;
		root->dlo = 1;
		root->dhi = 0;
		r = fat_stream_decode(fs, root, 0);
		if (r == ST_OK)
			r = fat_handle_alloc(fs, root);
		if (r != ST_OK) {
			if (root->ext)
				kfree(root->ext);
			kfree(root);
			kprintf("fat: %s: root directory unreadable (%d)\n",
				b->name, r);
			r = r == ST_NOMEM ? ST_NOMEM : ST_IO;
			goto fail_locked;
		}
		root->refs = 1; /* the instance's */
		fat_khash_insert_locked(fs, root);
		fs->root = root;
		fs->nstreams = 1;
	}
	if (g.fat_bits == 32) {
		fat_fsinfo_read(fs);
		if (fs->fsinfo_valid) {
			if (fs->fsinfo_next >= FAT_FIRST_CLUSTER &&
			    fs->fsinfo_next < fs->cluster_end)
				fs->alloc_cursor = fs->fsinfo_next;
			if (fs->opts.usefree &&
			    fs->fsinfo_free <= fs->cluster_count)
				fs->free_clusters = fs->fsinfo_free;
		}
		if (!from_backup)
			fat_check_backup(fs, &g);
	}
	/* A metadata read error at mount latches nothing: errors are counted
	 * from here on. */
	fs->errors_seen = 0;
	fs->ro_latched = 0;
	fat_meta_wunlock(fs);
	if (fs->was_dirty && !fs->ro)
		kprintf("fat: %s: volume was not properly unmounted; some data may be corrupt (run fsck.fat); it stays marked dirty\n",
			b->name);
	fat_shrinker_register(fs);
	*out = fs;
	return ST_OK;

fail_locked:
	fat_meta_wunlock(fs);
	fat_fs_free(fs);
	return r;
}

static int fat_fst_mount(vfs_mount_t *m, struct block_device *bdev,
			 unsigned long flags, const char *data)
{
	struct fat_opts o;
	fat_fs_t *fs;
	int r;

	if (!m || !bdev)
		return ST_INVALID;
	fat_opts_default(&o);
	if (fat_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	/* Something that does not even look like a FAT boot sector is not
	 * reported field by field (mount -t auto tries every type). */
	r = fat_instance_up(bdev, m, (flags & VFS_MS_RDONLY) != 0, &o,
			    !fat_probe_boot(bdev), &fs);
	if (r != ST_OK)
		return r;
	if (fs->ro && !(flags & VFS_MS_RDONLY)) {
		char why[256];

		fat_gates_text(fs->gates & ~(1u << FAT_RO_REQUESTED), why,
			       sizeof(why));
		kprintf("fat: %s: mounted read-only: %s\n", bdev->name, why);
		m->flags |= VFS_MS_RDONLY;
	}
	m->ops = &fat_vfs_ops;
	m->fs_data = fs;
	m->sb = &fs->sb;
	return ST_OK;
}

/* The end of an unmount: nothing is open, the VFS has stored the pages and
 * inodes.  Store what is left, mark the volume clean, let go of every cache
 * entry and free the instance. */
static int fat_fst_unmount(vfs_mount_t *m)
{
	fat_fs_t *fs = fat_fs_of(m);
	int r;

	if (!fs)
		return ST_INVALID;
	if (fs->is_root)
		return ST_BUSY;
	fat_meta_wlock(fs);
	r = fat_clean(fs);
	/* Whatever could not be stored is lost now: the device is going. */
	if (fs->dirty_head || fs->ndirw || fs->npf || fs->ndfat)
		fat_commit_abandon(fs);
	fat_meta_wunlock(fs);
	if (r != ST_OK)
		kprintf("fat: %s: could not store everything before unmount (%d)\n",
			fat_name_of(fs), r);
	vfs_sb_release_caches(&fs->sb);
	fat_fs_free(fs);
	m->fs_data = NULL;
	m->sb = NULL;
	return ST_OK;
}

/* Read-only <-> read-write and new options.  Going read-only stores
 * everything and marks the volume clean (the VFS refuses while files are
 * open for writing); going read-write is refused when a gate still holds.
 * Options that decide how names are stored or shown (codepage, iocharset,
 * shortname, fat) keep their values: the caches hold names made under them. */
static int fat_fst_remount(vfs_mount_t *m, unsigned long flags,
			   const char *data)
{
	fat_fs_t *fs = fat_fs_of(m);
	struct fat_opts o;
	int want_ro = (flags & VFS_MS_RDONLY) != 0;
	int r = ST_OK;

	if (!fs)
		return ST_INVALID;
	o = fs->opts;
	if (data && fat_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	o.codepage = fs->opts.codepage;
	o.iocharset = fs->opts.iocharset;
	o.uni_xlate = fs->opts.uni_xlate;
	o.shortname = fs->opts.shortname;
	o.fat_bits = fs->opts.fat_bits;
	fat_meta_wlock(fs);
	if (want_ro && !fs->ro) {
		r = fat_clean(fs);
		if (r != ST_OK && !fs->ro_latched) {
			fat_meta_wunlock(fs);
			return r;
		}
		fat_gate(fs, FAT_RO_REQUESTED);
		r = ST_OK;
	} else if (!want_ro && fs->ro) {
		unsigned left = fs->gates & ~(1u << FAT_RO_REQUESTED);

		/* The device's write protection is asked again. */
		if (blk_bdev_read_only(fs->bdev))
			left |= 1u << FAT_RO_DEVICE;
		else
			left &= ~(1u << FAT_RO_DEVICE);
		if (fs->ro_latched) {
			r = ST_ROFS;
		} else if (left) {
			char why[256];

			fat_gates_text(left, why, sizeof(why));
			kprintf("fat: %s: stays read-only: %s\n", fat_name_of(fs),
				why);
			r = ST_ROFS;
		} else {
			fs->gates = 0;
			fs->ro = 0;
		}
	}
	if (r == ST_OK)
		fs->opts = o;
	fat_meta_wunlock(fs);
	return r;
}

static int fat_fst_probe(struct block_device *bdev)
{
	return fat_probe_boot(bdev);
}

static const char *const fat_fst_aliases[] = { "fat32", "msdos", "fat",
					       "fat16", "fat12", NULL };

const vfs_fstype_t fat32_fstype = {
	.name = "vfat",
	.aliases = fat_fst_aliases,
	.needs_device = 1,
	.mount = fat_fst_mount,
	.unmount = fat_fst_unmount,
	.remount = fat_fst_remount,
	.show_options = fat_fst_show_options,
	.probe = fat_fst_probe,
};

/* ---- the root filesystem (storage bootstrap) -------------------------------- */

int fat32_mount(const block_device_t *bdev, fat32_fs_t *out)
{
	struct fat_opts o;
	fat_fs_t *fs;
	int r;

	if (!bdev || !out)
		return ST_INVALID;
	out->fs = NULL;
	fat_opts_default(&o);
	if (!fat_probe_boot((struct block_device *)bdev))
		return ST_NOT_FOUND;
	r = fat_instance_up((block_device_t *)bdev, NULL, 0, &o, 1, &fs);
	if (r != ST_OK)
		return r;
	if (fs->ro) {
		char why[256];

		fat_gates_text(fs->gates, why, sizeof(why));
		kprintf("fat: %s: root mounted read-only: %s\n", bdev->name, why);
	}
	out->fs = fs;
	return ST_OK;
}

void fat32_release(fat32_fs_t *out)
{
	if (!out || !out->fs)
		return;
	fat_fs_free(out->fs);
	out->fs = NULL;
}

int fat32_vfs_register_root(fat32_fs_t *out)
{
	fat_fs_t *fs;
	vfs_superblock_t *prev_sb = g_root_sb;
	int r;

	if (!out || !out->fs)
		return ST_INVALID;
	fs = out->fs;
	fs->is_root = 1;
	/* The caches' superblock for whole-system flushes (pagecache_sync). */
	g_root_sb = &fs->sb;
	r = vfs_register_root(&fat_vfs_ops, fs, &fs->sb, &fat32_fstype, "vfat");
	if (r != ST_OK) {
		/* A root is already there: it keeps what it published. */
		g_root_sb = prev_sb;
		fs->is_root = 0;
	}
	return r;
}
