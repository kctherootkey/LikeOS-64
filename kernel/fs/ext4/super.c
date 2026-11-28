// LikeOS - ext4 filesystem driver: mount, probe, mount options, error policy, statfs,
// the filesystem type and the root filesystem
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/fs/ext4/ext4_int.h>

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
_Static_assert(__builtin_offsetof(ext4_super_block, s_log_groups_per_flex) ==
		       0x174,
	       "s_log_groups_per_flex offset");
_Static_assert(__builtin_offsetof(ext4_super_block, s_backup_bgs) == 0x24C,
	       "s_backup_bgs offset");
_Static_assert(__builtin_offsetof(ext4_group_desc, bg_checksum) == 0x1E,
	       "bg_checksum offset");
_Static_assert(__builtin_offsetof(ext4_inode, i_checksum_hi) == 0x82,
	       "i_checksum_hi offset");

/* Load the entire group descriptor table into fs->gdt at mount time.  The
 * GDT is read on every inode access, so caching it in memory eliminates a
 * block read per inode lookup. */
static int ext4_load_gdt(ext4_fs_t *fs)
{
	if (fs->gdt) {
		kfree(fs->gdt);
		fs->gdt = 0;
	}
	/* The descriptor table (read as one run of blocks after the
	 * superblock) lies within the filesystem, whose block count the
	 * mount checked against the device: the allocation below is then
	 * bounded by what the device can hold. */
	{
		unsigned long per_blk = fs->block_size / fs->desc_size;
		unsigned long tbl_blocks =
			((unsigned long)fs->groups_count + per_blk - 1) /
			per_blk;
		if (fs->groups_count == 0 ||
		    fs->first_data_block + 1 + tbl_blocks > fs->blocks_count) {
			kprintf("ext4: %u group descriptors do not fit in the "
				"filesystem (%lu blocks)\n",
				fs->groups_count, fs->blocks_count);
			return ST_INVALID;
		}
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

/* ---- Errors found on the filesystem ------------------------------------
 *
 * Every corruption the driver finds goes through ext4_fs_error (or
 * ext4_bad_block for a block number from the disk that may not be used).
 * It is recorded in the superblock -- the "clean" bit cleared, the error
 * bit set so that the next e2fsck runs a full check, the count bumped, and
 * the first and the latest error described (time, inode, block, function,
 * line) -- and the errors= policy is applied.
 *
 * A damaged file is usually found damaged again on every block a reader
 * maps, so both reactions are bounded.  The messages are rate limited per
 * filesystem.  The superblock is written when this mount records its first
 * error -- that is when the state on disk changes -- and later errors only
 * update the record in memory, which reaches the disk at unmount; one
 * superblock write per block read would be the alternative.
 * ext4_write_super is intentionally NOT gated by the read-only latch so
 * that the record can be stored. */

/* At most EXT4_ERR_RL_BURST messages in EXT4_ERR_RL_INTERVAL seconds. */
#define EXT4_ERR_RL_INTERVAL 5u
#define EXT4_ERR_RL_BURST 10u

/* 1 if an error message may be printed now (then counted), 0 if it is held
 * back.  The first one of a new window says how many the last one held
 * back. */
static int ext4_err_ratelimit(ext4_fs_t *fs)
{
	uint64_t now = timer_ticks();
	uint64_t win = (uint64_t)EXT4_ERR_RL_INTERVAL * timer_get_frequency();
	unsigned missed = 0;
	uint64_t fl;
	int ok;

	spin_lock_irqsave(&fs->st->err_lock, &fl);
	if (!fs->st->err_rl_begin || now - fs->st->err_rl_begin >= win) {
		missed = fs->st->err_rl_missed;
		fs->st->err_rl_missed = 0;
		fs->st->err_rl_printed = 0;
		fs->st->err_rl_begin = now ? now : 1;
	}
	ok = fs->st->err_rl_printed < EXT4_ERR_RL_BURST;
	if (ok)
		fs->st->err_rl_printed++;
	else
		fs->st->err_rl_missed++;
	spin_unlock_irqrestore(&fs->st->err_lock, fl);
	if (missed)
		kprintf("ext4: %u error message(s) suppressed\n", missed);
	return ok;
}

/* The name of the function that found an error, as the superblock keeps
 * it: at most 32 bytes, zero-padded, not necessarily terminated. */
static void ext4_err_func(char dst[32], const char *func)
{
	unsigned i = 0;

	for (; func && func[i] && i < 32; i++)
		dst[i] = func[i];
	for (; i < 32; i++)
		dst[i] = 0;
}

/* Put an error into the superblock image.  Caller holds err_lock. */
static void ext4_err_record(ext4_fs_t *fs, unsigned long ino,
			    unsigned long blk, const char *func, unsigned line)
{
	ext4_super_block *es = &fs->sb_copy;
	uint64_t now = timer_get_epoch();

	es->s_state &= ~(uint16_t)EXT4_VALID_FS;
	es->s_state |= (uint16_t)EXT4_ERROR_FS;
	es->s_error_count++;
	if (!es->s_first_error_time && !es->s_first_error_time_hi) {
		es->s_first_error_time = (uint32_t)now;
		es->s_first_error_time_hi = (uint8_t)(now >> 32);
		es->s_first_error_ino = (uint32_t)ino;
		es->s_first_error_block = blk;
		es->s_first_error_line = line;
		es->s_first_error_errcode = EXT4_ERR_EFSCORRUPTED;
		ext4_err_func(es->s_first_error_func, func);
	}
	es->s_last_error_time = (uint32_t)now;
	es->s_last_error_time_hi = (uint8_t)(now >> 32);
	es->s_last_error_ino = (uint32_t)ino;
	es->s_last_error_block = blk;
	es->s_last_error_line = line;
	es->s_last_error_errcode = EXT4_ERR_EFSCORRUPTED;
	ext4_err_func(es->s_last_error_func, func);
}

/* Record an error in the superblock image, and store it there if this is
 * the first error of this mount (later ones are stored at unmount). */
static void ext4_record_error(ext4_fs_t *fs, unsigned long ino,
			      unsigned long blk, const char *func,
			      unsigned line)
{
	int store;
	uint64_t fl;

	spin_lock_irqsave(&fs->st->err_lock, &fl);
	ext4_err_record(fs, ino, blk, func, line);
	store = !fs->st->err_stored;
	fs->st->err_stored = 1;
	if (!store)
		fs->st->err_dirty = 1;
	spin_unlock_irqrestore(&fs->st->err_lock, fl);
	if (store) {
		ext4_write_super(fs); /* best-effort; ungated by latch */
		ext4_dev_sync(fs, "error-mark");
	}
}

/* Record an error and apply the errors= policy: remount read-only (the
 * journal aborted, then fs->read_only latched, so that all further writes
 * are refused), panic, or continue.  The failed operation itself returns
 * an error to its caller; this governs the filesystem-wide reaction.  The
 * policy applies on EVERY error, also on a disk that was already marked
 * errored when it was mounted (the mark says an earlier mount saw an error,
 * not that this one has reacted); only the "read-only" message is once per
 * mount. */
static void ext4_handle_error(ext4_fs_t *fs, unsigned long ino,
			      unsigned long blk, const char *func,
			      unsigned line)
{
	/* Before anything else can commit: what the running batch holds may
	 * be half of an operation that met the damage. */
	if (fs->errors_behavior != EXT4_ERRORS_CONTINUE)
		ext4_journal_abort(fs);
	ext4_record_error(fs, ino, blk, func, line);

	switch (fs->errors_behavior) {
	case EXT4_ERRORS_PANIC:
		kprintf("ext4: errors=panic -> halting\n");
		for (;;)
			__asm__ volatile("cli; hlt");
	case EXT4_ERRORS_CONTINUE:
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

void __ext4_fs_error(ext4_fs_t *fs, const char *what, unsigned long ino,
		     const char *func, unsigned line)
{
	if (!fs || !fs->st)
		return;
	if (ext4_err_ratelimit(fs)) {
		if (ino)
			kprintf("ext4: ERROR: %s (inode %lu)\n", what, ino);
		else
			kprintf("ext4: ERROR: %s\n", what);
	}
	ext4_handle_error(fs, ino, 0, func, line);
}

void __ext4_bad_block(ext4_fs_t *fs, unsigned long ino, const char *what,
		      unsigned long pbn, unsigned long count,
		      const struct ext4_err_site *site)
{
	if (!fs || !fs->st)
		return;
	if (ext4_err_ratelimit(fs)) {
		kprintf("ext4: %s: block %lu (%lu block(s)) is outside the "
			"data area or on filesystem metadata\n",
			what, pbn, count);
		if (ino)
			kprintf("ext4: ERROR: invalid block number (inode %lu)\n",
				ino);
		else
			kprintf("ext4: ERROR: invalid block number\n");
	}
	ext4_handle_error(fs, ino, pbn, site->func, site->line);
}

void ext4_store_error_info(ext4_fs_t *fs)
{
	uint64_t fl;
	int dirty;

	if (!fs || !fs->st)
		return;
	spin_lock_irqsave(&fs->st->err_lock, &fl);
	dirty = fs->st->err_dirty;
	fs->st->err_dirty = 0;
	spin_unlock_irqrestore(&fs->st->err_lock, fl);
	if (!dirty)
		return;
	ext4_write_super(fs);
	ext4_dev_sync(fs, "error-info");
}

int ext4_get_statfs(ext4_fs_t *fs, unsigned long *f_bsize, unsigned long *f_blocks,
		    unsigned long *f_bfree, unsigned long *f_bavail,
		    unsigned long *f_files,
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
	/* The total leaves out the filesystem's own structures, so a new
	 * filesystem shows (nearly) nothing used.  Available leaves out the
	 * blocks reserved for root and the small reserve kept back for the
	 * metadata an extent tree may still need (2 %, at most 4096). */
	unsigned long resv = fs->sb_copy.s_r_blocks_count_lo;
	if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
		resv |= (unsigned long)fs->sb_copy.s_r_blocks_count_hi << 32;
	if (fs->feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) {
		unsigned long r2 = fs->blocks_count / 50;
		resv += r2 < 4096 ? r2 : 4096;
	}
	if (f_blocks)
		*f_blocks = fs->blocks_count - fs->overhead_blocks;
	if (f_bfree)
		*f_bfree = bfree;
	if (f_bavail)
		*f_bavail = bfree > resv ? bfree - resv : 0;
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
 * Mount / probe / register
 * =================================================================== */

/* True if an ext4 superblock magic is present at (part_lba + sb offset). */
static int ext4_probe_offset(const block_device_t *bdev, unsigned long part_lba)
{
	ext4_super_block *sb =
		(ext4_super_block *)kalloc(sizeof(ext4_super_block));
	int ok = 0;

	if (!sb)
		return 0;
	if (ext4_sb_read_raw(bdev, part_lba, sb) == ST_OK &&
	    sb->s_magic == EXT4_SUPER_MAGIC)
		ok = 1;
	kfree(sb);
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

static int ext4_state_alloc(ext4_fs_t *fs);

/* The block count of the superblock: its high half counts only on a 64BIT
 * filesystem (elsewhere those bytes carry no meaning). */
static unsigned long ext4_sb_blocks_count(const ext4_super_block *sb)
{
	unsigned long n = sb->s_blocks_count_lo;
	if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
		n |= (unsigned long)sb->s_blocks_count_hi << 32;
	return n;
}

/* Is the superblock's geometry one this driver can use?  ST_OK, or
 * ST_INVALID with a message.  Every field checked here is used as a shift
 * count, a divisor or a size within a block, so a damaged or hostile
 * superblock is refused before any of it is used.  `ss': the device's
 * sector size; the filesystem starts at sector `part_lba' of `bdev'. */
static int ext4_sb_geometry_check(const ext4_super_block *sb, unsigned ss,
				  const block_device_t *bdev,
				  unsigned long part_lba)
{
	/* Blocks of 1 KiB (log 0) to 64 KiB (log 6). */
	if (sb->s_log_block_size > 6) {
		kprintf("ext4: invalid log block size %u; not mounted\n",
			(unsigned)sb->s_log_block_size);
		return ST_INVALID;
	}
	unsigned bs = 1024u << sb->s_log_block_size;
	if (bs < ss) {
		kprintf("ext4: block size %u is smaller than the device's "
			"sector size %u; not mounted\n",
			bs, ss);
		return ST_INVALID;
	}
	/* An inode slot never crosses a block: a power of two from 128 up
	 * to the block size. */
	if (sb->s_rev_level != 0) {
		unsigned is = sb->s_inode_size;
		if (is < EXT4_GOOD_OLD_INODE_SIZE || is > bs ||
		    (is & (is - 1)) != 0) {
			kprintf("ext4: unsupported inode size %u; "
				"not mounted\n",
				is);
			return ST_INVALID;
		}
	}
	/* Group descriptors of a 64-bit filesystem: 64 bytes (0 stands for
	 * 64).  The format allows larger powers of two, but each descriptor
	 * is kept (and checksummed) in an ext4_group_desc, which holds 64. */
	if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		unsigned ds = sb->s_desc_size;
		if (ds != 0 && (ds < 64 || ds > sizeof(ext4_group_desc) ||
				(ds & (ds - 1)) != 0)) {
			kprintf("ext4: unsupported group descriptor size %u; "
				"not mounted\n",
				ds);
			return ST_INVALID;
		}
	}
	/* A group's blocks and inodes each have a one-block bitmap. */
	if (sb->s_blocks_per_group == 0 || sb->s_blocks_per_group > 8ul * bs) {
		kprintf("ext4: invalid blocks per group %u; not mounted\n",
			(unsigned)sb->s_blocks_per_group);
		return ST_INVALID;
	}
	if (sb->s_inodes_per_group == 0 || sb->s_inodes_per_group > 8ul * bs) {
		kprintf("ext4: invalid inodes per group %u; not mounted\n",
			(unsigned)sb->s_inodes_per_group);
		return ST_INVALID;
	}
	unsigned long blocks = ext4_sb_blocks_count(sb);
	/* No more blocks than the device holds from the filesystem's start
	 * on (fewer is fine: a filesystem need not fill its device).  This
	 * also bounds the group count, and with it the descriptor table. */
	if (bdev->total_sectors) {
		unsigned long dev_blocks = 0;
		if (bdev->total_sectors > part_lba)
			dev_blocks = (bdev->total_sectors - part_lba) /
				     (bs / ss);
		if (blocks > dev_blocks) {
			kprintf("ext4: block count %lu exceeds the device "
				"(%lu blocks of %u bytes); not mounted\n",
				blocks, dev_blocks, bs);
			return ST_INVALID;
		}
	}
	if (sb->s_first_data_block >= blocks) {
		kprintf("ext4: first data block %u beyond the block count %lu; "
			"not mounted\n",
			(unsigned)sb->s_first_data_block, blocks);
		return ST_INVALID;
	}
	unsigned long span = blocks - sb->s_first_data_block;
	unsigned long groups = span / sb->s_blocks_per_group +
			       (span % sb->s_blocks_per_group != 0);
	if (groups > 0xFFFFFFFFul) {
		kprintf("ext4: %lu block groups are too many; not mounted\n",
			groups);
		return ST_INVALID;
	}
	return ST_OK;
}

/* The group descriptors, the system zone built from them and the journal
 * inode (ext4_check_descriptors, ext4_setup_system_zone, ext4_journal_check).
 * A filesystem failing any of these is not mounted. */
static int ext4_check_layout(ext4_fs_t *fs)
{
	int rc = ext4_check_descriptors(fs);

	if (rc == ST_OK)
		rc = ext4_setup_system_zone(fs, 0, 0);
	if (rc == ST_OK)
		rc = ext4_journal_check(fs);
	return rc;
}

/* The names of read-only compatible feature bits, comma separated. */
static void ext4_ro_compat_names(uint32_t bits, char *buf, unsigned cap)
{
	static const struct {
		uint32_t bit;
		const char *name;
	} tbl[] = {
		{ EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER, "sparse_super" },
		{ EXT4_FEATURE_RO_COMPAT_LARGE_FILE, "large_file" },
		{ 0x0004, "btree_dir" },
		{ EXT4_FEATURE_RO_COMPAT_HUGE_FILE, "huge_file" },
		{ EXT4_FEATURE_RO_COMPAT_GDT_CSUM, "uninit_bg" },
		{ EXT4_FEATURE_RO_COMPAT_DIR_NLINK, "dir_nlink" },
		{ EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE, "extra_isize" },
		{ 0x0080, "snapshot" },
		{ 0x0100, "quota" },
		{ 0x0200, "bigalloc" },
		{ EXT4_FEATURE_RO_COMPAT_METADATA_CSUM, "metadata_csum" },
		{ 0x0800, "replica" },
		{ 0x1000, "read-only" },
		{ 0x2000, "project" },
		{ 0x4000, "shared_blocks" },
		{ 0x8000, "verity" },
		{ EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT, "orphan_present" },
	};
	unsigned k = 0;

	buf[0] = '\0';
	for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
		if (!(bits & tbl[i].bit))
			continue;
		bits &= ~tbl[i].bit;
		int w = ksnprintf(buf + k, cap - k, "%s%s", k ? "," : "",
				  tbl[i].name);
		if (w < 0 || (unsigned)w >= cap - k)
			return;
		k += (unsigned)w;
	}
	if (bits)
		ksnprintf(buf + k, cap - k, "%sunknown 0x%x", k ? "," : "",
			  bits);
}

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

	/* The ext4 superblock is at byte offset 1024 relative to the
	 * partition start (inside the first sector on a device with sectors
	 * larger than 1024 bytes). */
	uint8_t *raw = (uint8_t *)kalloc(sizeof(ext4_super_block));
	if (!raw)
		return ST_NOMEM;
	if (ext4_sb_read_raw(bdev, part_lba, raw) != ST_OK) {
		kfree(raw);
		return ST_IO;
	}
	ext4_super_block *sb = (ext4_super_block *)raw;
	if (sb->s_magic != EXT4_SUPER_MAGIC) {
		kfree(raw);
		return ST_NOT_FOUND;
	}
	/* The geometry is checked before any of it is used in a shift, a
	 * division or a buffer size. */
	if (ext4_sb_geometry_check(sb, ss, bdev, part_lba) != ST_OK) {
		kfree(raw);
		return ST_INVALID;
	}
	{
		/* What mount(2) asked for (ext4_mount_opts) outlives the
		 * reset: norecovery and the write-protect rule depend on it. */
		unsigned opts = out->mnt_opts;

		mm_memset(out, 0, sizeof(*out));
		out->mnt_opts = opts;
	}
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
	out->inodes_per_group = sb->s_inodes_per_group;
	out->blocks_per_group = sb->s_blocks_per_group;
	out->inodes_count = sb->s_inodes_count;
	out->blocks_count = ext4_sb_blocks_count(sb);
	out->first_data_block = sb->s_first_data_block;
	out->feature_incompat = sb->s_feature_incompat;
	out->feature_ro_compat = sb->s_feature_ro_compat;
	out->feature_compat = sb->s_feature_compat;
	out->first_ino = (sb->s_rev_level == 0) ? EXT4_GOOD_OLD_FIRST_INO :
						  sb->s_first_ino;
	out->journal_inum = sb->s_journal_inum;
	out->read_only = 0; /* writes enabled */
	/* Read-only because a checksum failed (not merely because of a
	 * feature this driver does not write): no journal replay either. */
	int csum_ro = 0;
	/* errors= policy for runtime corruption (ext4_fs_error).  The mount-time
     * default is remount-ro on metadata corruption regardless of the on-disk
     * s_errors hint (mke2fs stamps s_errors=continue by default), since
     * leaving a corrupt filesystem writable risks compounding the damage:
     * default to the safe remount-ro, honouring only an explicit panic
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
	/* metadata_csum supersedes gdt_csum where both are set. */
	out->has_gd_csum = out->has_metadata_csum ||
			   (out->feature_ro_compat &
			    EXT4_FEATURE_RO_COMPAT_GDT_CSUM) != 0;
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
			csum_ro = 1;
		} else
			kprintf("ext4: metadata_csum on; superblock csum verified\n");
	}

	kfree(raw);

	/* Reject incompat features we cannot safely read.  inline_data is one
	 * of them: a small file's bytes (or a small directory's entries) live
	 * in the inode itself, which neither the read paths (they map blocks)
	 * nor the write paths (they would turn i_block into a block map over
	 * the data) know -- such a filesystem would read as I/O errors. */
	uint32_t known_incompat =
		EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_RECOVER |
		EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT |
		EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_META_BG |
		EXT4_FEATURE_INCOMPAT_CSUM_SEED;
	if (out->feature_incompat & ~known_incompat) {
		kprintf("ext4: unsupported incompat features 0x%x\n",
			out->feature_incompat & ~known_incompat);
		return ST_UNSUPPORTED;
	}

	/* Read-only-compatible features this driver does not maintain
	 * (bigalloc, quota, project, verity, a pending orphan file, the
	 * readonly flag, ...): the filesystem can be read, but a write that
	 * ignored them would damage it -- mounted read-only. */
	if (out->feature_ro_compat & ~EXT4_RO_COMPAT_WRITABLE) {
		uint32_t rest = out->feature_ro_compat & ~EXT4_RO_COMPAT_WRITABLE;
		char names[160];

		ext4_ro_compat_names(rest, names, sizeof(names));
		kprintf("ext4: read-only compatible features 0x%x (%s) not "
			"supported for writing -> mounting read-only\n",
			rest, names);
		if (rest & EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT)
			kprintf("ext4: orphan_present: the filesystem was not "
				"unmounted cleanly (removed or switched off while "
				"in use) while files deleted but still open were "
				"listed in its orphan file; freeing them is left to "
				"a mount that processes the orphan file, which this "
				"driver does not\n");
		out->read_only = 1;
	}

	ext4_inode_cache_flush(fs); /* reset the parsed-inode cache */
	ext4_mbc_invalidate(fs);
	if (ext4_load_gdt(out) != ST_OK) {
		kprintf("ext4: failed to load group descriptor table\n");
		return ST_IO;
	}

	/* Verify every group descriptor's checksum (metadata_csum or gdt_csum
     * form).  A mismatch degrades the mount to read-only; never rejects. */
	if (out->has_gd_csum) {
		unsigned bad = 0;
		for (unsigned int g = 0; g < out->groups_count; g++)
			if (ext4_gd_csum(out, g, &out->gdt[g]) !=
			    out->gdt[g].bg_checksum)
				bad++;
		if (bad) {
			/* A corrupt group descriptor would mis-place bitmaps / inode tables —
             * degrade to read-only and mark errored rather than write through it. */
			kprintf("ext4: ERROR %u/%u group-desc checksum mismatch "
				"-> mounting read-only\n",
				bad, out->groups_count);
			out->sb_copy.s_state &= ~(uint16_t)EXT4_VALID_FS;
			out->sb_copy.s_state |= (uint16_t)EXT4_ERROR_FS;
			out->read_only = 1;
			csum_ro = 1;
		} else
			kprintf("ext4: all %u group-desc csums verified\n",
				out->groups_count);
	}

	/* Where the groups keep their bitmaps and inode tables, and which
	 * blocks the journal maps, is checked before any of it is read or
	 * written: from here on these are the blocks every write may go to. */
	{
		int lc = ext4_check_layout(out);
		if (lc != ST_OK)
			return lc;
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
		   ext4_journal_recover(out, NULL, 1) == ST_ROFS) {
		/* The journal holds committed transactions and the device
		 * cannot be written, so it cannot be replayed. */
		if (out->mnt_opts & EXT4_MNT_WP_STRICT) {
			/* Mounting without the replay would show a
			 * filesystem missing changes it committed. */
			kprintf("ext4: the journal needs recovery and the device is "
				"write-protected: not mounted (norecovery mounts it "
				"read-only without replaying, possibly inconsistent)\n");
			return ST_ROFS;
		}
		/* The root at boot keeps going: read-only, without the
		 * replay, as norecovery would mount it -- never written, so
		 * the journal is there for a mount that can replay it. */
		kprintf("ext4: the journal needs recovery and the device is "
			"write-protected: mounted read-only without replaying "
			"it -- the filesystem may read inconsistent\n");
		out->mnt_opts |= EXT4_MNT_NOLOAD;
		out->read_only = 1;
	} else if (out->journal_inum != 0 && !csum_ro &&
		   (out->mnt_opts & EXT4_MNT_ASK_REPLAY) &&
		   ext4_journal_recover(out, NULL, 1) == ST_ROFS) {
		/* recovery=ask: the caller decides, and replays it with a
		 * mount without the option. */
		kprintf("ext4: the journal needs to be replayed: not mounted "
			"(a mount without recovery=ask replays it)\n");
		return ST_UCLEAN;
	} else if (out->journal_inum != 0) {
		int replayed = 0;
		/* A filesystem mounted read-only only for a read-only
		 * compatible feature this driver does not write is still
		 * replayed: the replay writes nothing but the blocks the
		 * journal holds, whatever the features. */
		int ro_was = out->read_only;
		if (!csum_ro)
			out->read_only = 0;
		int rr = ext4_journal_recover(out, &replayed, 0);
		if (ro_was)
			out->read_only = 1; /* an error latch stays as well */
		if (rr != ST_OK) {
			/* Mounting a filesystem whose committed transactions
			 * could not be applied would show -- and build on --
			 * a state the journal says is out of date.  Not
			 * mounted, read-only neither (norecovery mounts it
			 * read-only as it is); RECOVER stays set, so the next
			 * attempt replays from the start again. */
			kprintf("ext4: journal replay failed (%d): not mounted "
				"(norecovery mounts it read-only without "
				"replaying, possibly inconsistent)\n",
				rr);
			return rr == ST_NOMEM ? ST_NOMEM : ST_IO;
		}
		if (replayed) {
			/* Replay rewrote final metadata under us; drop caches that
			 * were populated before/by replay so later reads see the
			 * recovered state. */
			ext4_mbc_invalidate(fs);
			ext4_inode_cache_flush(fs);
			ext4_load_gdt(
				out); /* GDT blocks may have been replayed   */
			/* ... and with them the group layout. */
			int lc = ext4_check_layout(out);
			if (lc != ST_OK)
				return lc;
		}
		/* Clear RECOVER (if it was set) now the journal is
		 * clean again -- on a device that can be written. */
		if ((out->feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) &&
		    !(dev_ro && (out->mnt_opts & EXT4_MNT_WP_STRICT))) {
			out->feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
			out->sb_copy.s_feature_incompat &=
				~EXT4_FEATURE_INCOMPAT_RECOVER;
			ext4_write_super(out);
			ext4_dev_sync(out, "mount-clear-recover");
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
     * plain (mke2fs ships it that way) the journal becomes csum-v3 with the
     * first transaction committed (ext4_journal_upgrade), so our own images
     * get csum-protected journaling.  Must run after recovery so j_sequence
     * reflects the post-recovery journal state (and the log is clean before
     * any upgrade). */
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

				/* A metadata_csum filesystem gets a csum-v3
				 * journal: mkfs leaves it plain (or with
				 * CSUM_V3 but no checksum type, which journal
				 * readers reject).  Only noted here and done
				 * by the first transaction this mount commits
				 * (ext4_journal_upgrade), so that a mount
				 * which changes nothing writes nothing.  Only
				 * with a clean log -- true after recovery and
				 * on a clean image. */
				int type_ok =
					(out->j_sb_buf[JBD2_SB_CSUM_TYPE_OFF] ==
					 JBD2_CRC32C_CHKSUM);
				if (out->j_enabled && out->has_metadata_csum &&
				    be32(jsb->s_start) == 0 &&
				    (!out->j_csum3 || !type_ok))
					fs->st->j_upgrade = 1;
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
	out->overhead_blocks = ext4_calc_overhead(out);
	return ST_OK;
}

/* ---- The per-filesystem state: allocation and release ------------------- */

static int ext4_state_alloc(ext4_fs_t *fs)
{
	struct ext4_state *st = (struct ext4_state *)kalloc(sizeof(*st));

	if (!st)
		return ST_NOMEM;
	mm_memset(st, 0, sizeof(*st));
	ext4_rwsem_init(&st->meta, "ext4_meta", st->meta_rh, EXT4_META_TRACK);
	for (int i = 0; i < EXT4_ILOCKS; i++)
		ext4_rwsem_init(&st->ilock[i], "ext4_ino", st->ilock_rh[i],
				EXT4_ILOCK_TRACK);
	for (int i = 0; i < EXT4_BGLOCKS; i++)
		ext4_rwsem_init(&st->bglock[i], "ext4_bg", 0, 0);
	spinlock_init(&st->mbc_lock, "ext4_mbc");
	spinlock_init(&st->bpool_lock, "ext4_bpool");
	spinlock_init(&st->di_lock, "ext4_di");
	spinlock_init(&st->ic_lock, "ext4_ic");
	spinlock_init(&st->ord_lock, "ext4_ordered");
	spinlock_init(&st->err_lock, "ext4_err");
	st->gd_lo = (unsigned)-1;
	st->gd_seen_lo = (unsigned)-1;
	st->cwd_ino = EXT4_ROOT_INO;
	fs->st = st;
	if (ext4_mbc_init(fs) != ST_OK) {
		fs->st = NULL;
		kfree(st);
		return ST_NOMEM;
	}
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
		if (st->pf)
			kfree(st->pf);
		if (st->sysz)
			kfree(st->sysz);
		long cached = 0;
		ext4_mbc_destroy(fs);
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
	ext4_record_error(fs, 0, 0, __func__, __LINE__);
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
 * "noload") skips the journal replay, "recovery=ask" refuses a mount that
 * would replay it (ST_UCLEAN).  The generic ones are the VFS's, and
 * the rest are accepted and change nothing. */
static unsigned ext4_fst_opts(const char *data)
{
	const char *cur = data, *key, *val;
	unsigned opts = EXT4_MNT_WP_STRICT;
	char tmp[64];

	while (data && vfs_next_option(&cur, tmp, sizeof(tmp), &key, &val))
		if (!kstrcmp(key, "norecovery") || !kstrcmp(key, "noload"))
			opts |= EXT4_MNT_NOLOAD;
		else if (!kstrcmp(key, "recovery") && val &&
			 !kstrcmp(val, "ask"))
			opts |= EXT4_MNT_ASK_REPLAY;
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
	/* Error details recorded after the superblock was last written. */
	ext4_store_error_info(fs);
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
		/* Features it cannot write (see ext4_mount_sb). */
		if (fs->feature_ro_compat & ~EXT4_RO_COMPAT_WRITABLE)
			return ST_ROFS;
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
