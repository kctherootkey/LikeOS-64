/* LikeOS - NTFS filesystem driver
 *
 * Mount, remount, unmount, $Volume flags, statfs, the operation tables.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#define NTFS_INTERNAL 1 /* the driver-internal section of ntfs.h */
#include <kernel/fs/ntfs/ntfs.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/dev/block/blkdev.h>
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
#include <kernel/ke/uaccess.h>
#include <kernel/fs/pathbuf.h>
#include <kernel/fs/ntfs/ntfs_int.h>

/* ===========================================================================
 * S22, S23  Mount helpers: option values, boot sector decoding, $LogFile
 * restart pages
 *
 * Option parsing, the boot sector read and the $LogFile check are S22
 * (below); the clean log writer, S23 (logfile.c).
 * =========================================================================== */

int ntfs_opt_num(const char *v, unsigned base, uint64_t *out)
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
		if (d >= base || n > (~0ULL - d) / base)
			return 0;
		n = n * base + d;
	}
	*out = n;
	return 1;
}

/* A boolean option: bare, or =1/0, yes/no, true/false, on/off. */
int ntfs_opt_bool(const char *v, int *out)
{
	if (!v || ntfs_streq(v, "1") || ntfs_streq(v, "yes") ||
	    ntfs_streq(v, "true") || ntfs_streq(v, "on")) {
		*out = 1;
		return 1;
	}
	if (ntfs_streq(v, "0") || ntfs_streq(v, "no") ||
	    ntfs_streq(v, "false") || ntfs_streq(v, "off")) {
		*out = 0;
		return 1;
	}
	return 0;
}

/* Decode a "clusters per record" byte: > 0 clusters, < 0 2^-n bytes. */
uint64_t ntfs_rec_size(int8_t c, uint32_t cluster)
{
	if (c > 0)
		return (uint64_t)(uint8_t)c * cluster;
	if (c < -31 || c > -9)
		return 0;
	return 1ULL << (unsigned)(-c); /* an unsigned shift */
}

/* Cluster size encoded in the boot sector, 0 when invalid. */
uint64_t ntfs_boot_cluster(uint32_t bps, uint8_t spc)
{
	if (spc == 0)
		return 0;
	if (spc >= 0xF4)
		return (uint64_t)bps << (256 - spc); /* sectors: 2^(256-v) */
	if (spc > 0x80 || (spc & (spc - 1)))
		return 0;
	return (uint64_t)bps * spc;
}

/* Does `b' (one boot sector, at least 512 bytes) describe an NTFS volume?
 * Only fields every valid volume has.  The full geometry check is
 * ntfs_boot_geometry(). */
int ntfs_boot_sane(const uint8_t *b)
{
	const ntfs_boot_sector_t *bs = (const ntfs_boot_sector_t *)b;
	uint32_t bps;
	uint64_t cluster;

	if (b[3] != 'N' || b[4] != 'T' || b[5] != 'F' || b[6] != 'S' ||
	    b[7] != ' ' || b[8] != ' ' || b[9] != ' ' || b[10] != ' ')
		return 0;
	if (b[510] != 0x55 || b[511] != 0xAA)
		return 0;
	bps = ntfs_le16(b + 0x0B);
	if (bps < 256 || bps > 4096 || (bps & (bps - 1)))
		return 0;
	cluster = ntfs_boot_cluster(bps, b[0x0D]);
	if (cluster < bps || cluster > (2UL << 20))
		return 0;
	if (ntfs_le64(b + 0x28) == 0)
		return 0;
	if (ntfs_le64(b + 0x30) == 0 || ntfs_le64(b + 0x38) == 0)
		return 0;
	if (ntfs_rec_size(bs->clusters_per_mft_record, (uint32_t)cluster) == 0)
		return 0;
	return 1;
}

/* The sequence-number width of LSNs in a log of `size' bytes: an LSN is
 * (sequence << file_bits) | (offset >> 3); with n the bit length of the
 * file size, the sequence number gets 67 - n bits (the format fixes this
 * relation, and readers check it). */
uint32_t ntfs_log_seq_bits(uint64_t size)
{
	uint32_t bits = 0;

	while (size) {
		size >>= 1;
		bits++;
	}
	return 67 - bits;
}

/* Check one restart page at `buf' (`len' bytes available).  Returns
 * ST_OK and *clean / *lsn, ST_NOT_FOUND when it is not a restart page,
 * ST_IO when it is a damaged one. */
int ntfs_rstr_check(const uint8_t *buf, uint32_t len, uint32_t *psize,
		    int *clean, int64_t *lsn)
{
	const ntfs_restart_page_header_t *rp =
		(const ntfs_restart_page_header_t *)buf;
	const ntfs_restart_area_t *ra;
	uint32_t sps, ra_off, ca_off, ra_len, usa_end, clients;
	uint8_t *copy;
	int r = ST_IO;

	if (len < 512)
		return ST_NOT_FOUND;
	if (rp->magic != NTFS_MAGIC_RSTR && rp->magic != NTFS_MAGIC_CHKD)
		return ST_NOT_FOUND;
	sps = rp->system_page_size;
	/* System and log page sizes: powers of two of 512 bytes at least;
	 * the restart page must fit what was read; the expected USA
	 * count is computed in 32 bits. */
	if (sps < 512 || sps > 65536 || !ntfs_is_pow2(sps) ||
	    rp->log_page_size < 512 || !ntfs_is_pow2(rp->log_page_size) ||
	    sps > len)
		return ST_IO;
	if ((uint32_t)rp->usa_count != sps / NTFS_BLOCK_SIZE + 1 ||
	    (rp->usa_ofs & 1))
		return ST_IO;
	usa_end = (uint32_t)rp->usa_ofs + (uint32_t)rp->usa_count * 2U;
	if (usa_end > NTFS_BLOCK_SIZE - 2)
		return ST_IO;
	ra_off = rp->restart_area_offset;
	if ((ra_off & 7) || ra_off < usa_end ||
	    ra_off + NTFS_RESTART_AREA_HDR > sps)
		return ST_IO;
	if (!((rp->major_ver == 1 && rp->minor_ver == 1) ||
	      (rp->major_ver == 2 && rp->minor_ver == 0)))
		return ST_IO;
	*psize = sps;
	if (rp->magic == NTFS_MAGIC_CHKD) {
		*clean = 0;
		*lsn = 0;
		return ST_OK;
	}
	copy = (uint8_t *)kalloc(sps);
	if (!copy)
		return ST_NOMEM;
	mm_memcpy(copy, buf, sps);
	if (ntfs_mst_fixup(copy, sps) != ST_OK)
		goto out;
	ra = (const ntfs_restart_area_t *)(copy + ra_off);
	ca_off = ra->client_array_offset;
	clients = ra->log_clients;
	/* All of it in 32 bits. */
	ra_len = ca_off + clients * NTFS_LOG_CLIENT_RECORD;
	if ((ca_off & 7) || ca_off < NTFS_RESTART_AREA_HDR - 8 ||
	    clients > 0xFFFE || ra_off + ra_len > sps ||
	    (uint32_t)ra->restart_area_length < ra_len)
		goto out;
	if ((ra->client_free_list != NTFS_LOGFILE_NO_CLIENT &&
	     ra->client_free_list >= clients) ||
	    (ra->client_in_use_list != NTFS_LOGFILE_NO_CLIENT &&
	     ra->client_in_use_list >= clients))
		goto out;
	*clean = ra->client_in_use_list == NTFS_LOGFILE_NO_CLIENT ||
		 (ra->flags & NTFS_RESTART_VOLUME_IS_CLEAN);
	*lsn = ra->current_lsn;
	r = ST_OK;
out:
	kfree(copy);
	return r;
}

/* 1 once mount() brings up a working instance (ntfs.h, section 6).  While 0,
 * probe() answers 0 so that "auto" never picks this driver, and mount()
 * refuses with ST_UNSUPPORTED. */
#define NTFS_DRIVER_READY 1

/* ---- $Volume flags ----
 *
 * Read-modify-write of the flag word under the metadata lock: the caller
 * passes the bits to set and the bits to clear, the current word is read
 * from the record, and the record is written only when the word changes.
 * Hibernated volumes are never written. */
static int ntfs_vol_flags(ntfs_fs_t *fs, uint16_t set, uint16_t clear)
{
	uint8_t *rec;
	ntfs_attr_record_t *a;
	ntfs_volume_information_t *vi;
	uint16_t nf;
	int r;

	ntfs_assert_meta_excl(fs);
	if (fs->hibernated)
		return ST_OK;
	rec = (uint8_t *)kalloc(fs->rec_size);
	if (!rec)
		return ST_NOMEM;
	r = ntfs_mft_read(fs, NTFS_FILE_VOLUME, rec);
	if (r != ST_OK)
		goto out;
	a = ntfs_rec_attr(fs, rec, NTFS_FILE_VOLUME,
			  NTFS_AT_VOLUME_INFORMATION, 0, 0, 0);
	if (!a || a->non_resident ||
	    a->data.resident.value_length < sizeof(*vi)) {
		r = ntfs_fs_error(fs, NTFS_FILE_VOLUME,
				  "$VOLUME_INFORMATION not in the base record");
		goto out;
	}
	vi = (ntfs_volume_information_t *)((uint8_t *)a +
					   a->data.resident.value_offset);
	nf = (uint16_t)((vi->flags | set) & ~clear);
	if (nf == vi->flags) {
		fs->vol_flags = nf;
		r = ST_OK;
		goto out;
	}
	vi->flags = nf;
	r = ntfs_mft_write_ex(fs, NTFS_FILE_VOLUME, rec,
			      (clear & NTFS_VOLUME_IS_DIRTY) != 0);
	if (r == ST_OK)
		fs->vol_flags = nf;
out:
	kfree(rec);
	return r;
}

/* ---- arming (ntfs.h section 3) ----
 *
 * Called by the chokepoint before the first write of a read-write mount,
 * with the metadata lock exclusive: empty $LogFile, set VOLUME_IS_DIRTY,
 * flush the device -- in that order, so that by the time the first real
 * change reaches the device, the volume says it is being changed.  A
 * failure latches the mount read-only before anything else is written. */
int ntfs_arm(ntfs_fs_t *fs)
{
	int r;

	if (fs->armed)
		return ST_OK;
	if (!ntfs_writable(fs) || fs->hibernated)
		return ST_ROFS;
	fs->arming = 1;
	r = ntfs_logfile_reset(fs);
	if (r == ST_OK)
		r = ntfs_vol_flags(fs, NTFS_VOLUME_IS_DIRTY, 0);
	fs->arming = 0;
	if (r != ST_OK) {
		ntfs_latch(fs, "the volume could not be marked dirty");
		return ST_ROFS;
	}
	fs->armed = 1;
	r = ntfs_dev_flush(fs);
	if (r != ST_OK)
		return ST_ROFS;
	return ST_OK;
}

/* ---- the clean-unmount sequence ---- */

/* Commit everything; with `clean', clear the dirty flag when nothing
 * failed and no error was ever recorded.  ST_OK only when everything
 * reached the device. */
static int ntfs_settle(ntfs_fs_t *fs, int clean)
{
	int r, rf;

	r = ntfs_commit_all(fs);
	if (!fs->armed)
		return r; /* never wrote: nothing to flush or clear */
	ntfs_meta_wlock(fs);
	rf = ntfs_dev_flush(fs);
	if (rf == ST_OK && fs->disc_n)
		ntfs_discard_run(fs); /* frees of the commits above */
	if (r == ST_OK)
		r = rf;
	if (clean && r == ST_OK && !fs->errors && !fs->latched_ro &&
	    fs->io_errs == 0) {
		/* A clean log first, then the clean flag -- unless the
		 * volume was dirty before this mount (a `force' mount): that
		 * flag is not ours to clear, the check it asks for has not
		 * run. */
		r = ntfs_logfile_write_clean(fs);
		if (r == ST_OK && !fs->was_dirty && !fs->keep_dirty)
			r = ntfs_vol_flags(fs, 0, NTFS_VOLUME_IS_DIRTY);
		if (r == ST_OK)
			r = ntfs_dev_flush(fs);
		if (r == ST_OK)
			fs->armed = 0; /* the next change arms again */
	}
	ntfs_meta_wunlock(fs);
	if (fs->errors || fs->latched_ro)
		return r == ST_OK ? ST_IO : r;
	return r;
}

/* ---- statfs ---- */

/* Count the clear bits of a bitmap stream below `limit' bits (the
 * scan is bounded by the volume, not by the bitmap's size). */
int ntfs_bitmap_free(ntfs_fs_t *fs, ntfs_stream_t *st, uint64_t limit,
		     uint64_t *free_out)
{
	uint64_t bytes = (limit + 7) / 8, free = 0;
	uint8_t *buf;
	int r = ST_OK;

	*free_out = 0;
	if (!st)
		return ST_IO;
	if (bytes > st->data_size)
		bytes = st->data_size;
	buf = (uint8_t *)kalloc(PAGE_SIZE);
	if (!buf)
		return ST_NOMEM;
	for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
		uint64_t n = ntfs_min64(PAGE_SIZE, bytes - off);

		r = ntfs_stream_pread(fs, st, off, n, buf, 0);
		if (r != ST_OK)
			break;
		for (uint64_t i = 0; i < n; i++) {
			uint64_t bit0 = (off + i) * 8;
			uint8_t b = buf[i];

			for (unsigned k = 0; k < 8; k++)
				if (bit0 + k < limit && !(b & (1U << k)))
					free++;
		}
	}
	kfree(buf);
	*free_out = free;
	return r;
}

static int ntfs_statfs(vfs_mount_t *m, struct vfs_statfs *out)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	uint64_t fc = 0, fr = 0, fl;
	int r = ST_OK, valid;

	if (!fs || !out)
		return ST_INVALID;
	spin_lock_irqsave(&fs->sf_lock, &fl);
	valid = fs->sf_valid;
	fc = fs->sf_free_clusters;
	fr = fs->sf_free_records;
	spin_unlock_irqrestore(&fs->sf_lock, fl);
	if (!valid) {
		ntfs_meta_rlock(fs);
		r = ntfs_bitmap_free(fs, fs->st_bitmap, fs->nr_clusters, &fc);
		if (r == ST_OK)
			r = ntfs_bitmap_free(fs, fs->st_mftbmp, fs->mft_records,
					     &fr);
		ntfs_meta_runlock(fs);
		if (r != ST_OK)
			return ST_IO;
		spin_lock_irqsave(&fs->sf_lock, &fl);
		fs->sf_free_clusters = fc;
		fs->sf_free_records = fr;
		fs->sf_valid = 1;
		spin_unlock_irqrestore(&fs->sf_lock, fl);
	}
	/* Clusters promised to delayed allocation are not free. */
	fc = fc > fs->resv_clusters ? fc - fs->resv_clusters : 0;
	out->f_type = NTFS_SUPER_MAGIC;
	out->f_bsize = fs->cluster_size;
	out->f_frsize = fs->cluster_size;
	out->f_blocks = fs->nr_clusters;
	out->f_bfree = fc;
	out->f_bavail = fc;
	/* Files: the MFT's records plus as many as the free clusters could
	 * still hold, as other NTFS implementations report it. */
	out->f_files = fs->mft_records +
		       ((fc << fs->cluster_bits) >> fs->rec_bits);
	out->f_ffree = fr + ((fc << fs->cluster_bits) >> fs->rec_bits);
	out->f_fsid = fs->serial;
	out->f_namelen = NTFS_MAX_NAME_LEN;
	return ST_OK;
}

/* ---- sync / fsync / misc ---- */

/* fsync: the stream's data and attribute committed, then the device
 * cache flushed and its error reported.  Directory
 * changes are written through already: only the flush. */
static int ntfs_fsync(vfs_file_t *f)
{
	ntfs_file_t *nf = ntfs_nf(f);
	ntfs_fs_t *fs;
	int r = ST_OK, rf;

	if (!nf)
		return ST_INVALID;
	fs = nf->fs;
	if (nf->st && !nf->is_dir &&
	    ((nf->st->flags & (NTFS_ST_META | NTFS_ST_DIRTY)) || nf->st->err)) {
		r = ntfs_commit_locked(fs, nf->st);
		nf->st->err = 0; /* reported once */
	}
	if (!fs->armed)
		return r;
	ntfs_meta_wlock(fs);
	rf = ntfs_dev_flush(fs);
	ntfs_meta_wunlock(fs);
	return r != ST_OK ? r : rf;
}

/* sync(2) / syncfs: everything pending committed and flushed; the dirty
 * flag stays (set-only when errors were recorded: it is set since
 * the mount armed). */
static int ntfs_sync(vfs_mount_t *m)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);

	if (!fs)
		return ST_INVALID;
	if (fs->ro)
		return ST_OK;
	return ntfs_settle(fs, 0);
}

static int ntfs_setid_clean(vfs_file_t *f, int mark)
{
	ntfs_file_t *nf = ntfs_nf(f);
	ic_inode_t *ic = nf ? (ic_inode_t *)nf->inode : NULL;

	if (!ic)
		return 1;
	if (mark) {
		ic->flags |= (uint32_t)IC_SETID_CLEAN;
		return 1;
	}
	return (ic->flags & IC_SETID_CLEAN) ? 1 : 0;
}

static int ntfs_release_locks_for_task(vfs_mount_t *m, uint64_t task_id)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	int released = 0;

	if (!fs)
		return 0;
	released |= ntfs_rwsem_release_owner(&fs->meta, task_id);
	released |= ntfs_rwsem_release_owner(&fs->alloc, task_id);
	for (int i = 0; i < NTFS_FLOCKS; i++)
		released |= ntfs_rwsem_release_owner(&fs->flock[i], task_id);
	if (fs->bdev && fs->bdev->release_locks_for_task)
		released |= fs->bdev->release_locks_for_task(fs->bdev, task_id);
	return released;
}

const vfs_ops_t ntfs_vfs_ops = {
	.open = ntfs_open,
	.open_mode = ntfs_open_mode,
	.stat = ntfs_stat,
	.lstat = ntfs_lstat,
	.fstat = ntfs_fstat,
	.read = ntfs_read,
	.read_at = ntfs_read_at,
	.seek = ntfs_seek,
	.readdir = ntfs_readdir,
	.chdir = ntfs_chdir,
	.close = ntfs_close,
	.readlink = ntfs_readlink,
	.statfs = ntfs_statfs,
	.getxattr = ntfs_getxattr,
	.listxattr = ntfs_listxattr,
	.fgetxattr = ntfs_fgetxattr,
	.flistxattr = ntfs_flistxattr,
	.getxattr_ino = ntfs_getxattr_ino,
	.inode_flags = ntfs_inode_flags,
	.fsync = ntfs_fsync,
	.sync = ntfs_sync,
	.setid_clean = ntfs_setid_clean,
	.release_locks_for_task = ntfs_release_locks_for_task,
	.write = ntfs_write,
	.write_at = ntfs_write_at,
	.truncate = ntfs_truncate,
	.unlink = ntfs_unlink,
	.rename = ntfs_rename,
	.mkdir = ntfs_mkdir,
	.rmdir = ntfs_rmdir,
	.mknod = ntfs_mknod,
	.symlink = ntfs_symlink,
	.link = ntfs_link,
	.chmod = ntfs_chmod,
	.chown = ntfs_chown,
	.fchmod = ntfs_fchmod,
	.fchown = ntfs_fchown,
	.utimens = ntfs_utimens,
	.futimens = ntfs_futimens,
	.setxattr = ntfs_setxattr,
	.removexattr = ntfs_removexattr,
	.fsetxattr = ntfs_fsetxattr,
	.fremovexattr = ntfs_fremovexattr,
};

/* ===========================================================================
 * S22  Mount
 *
 * Order (each step validates what it reads; a failure either refuses the
 * mount -- the volume cannot be read meaningfully -- or is recorded as a
 * mount error, which keeps the mount read-only):
 *
 *   1. boot sector: OEM id, signature, bytes per sector a power of two in
 *      [256, 4096] and a multiple of the device's sector, sectors
 *      per cluster incl. the 2^n encoding of large clusters, cluster
 *      <= 2 MB, MFT / index record sizes without undefined shifts,
 *      MFT record a power of two in [512, 4096], LCNs of $MFT / $MFTMirr
 *      inside the volume, the volume inside the device;
 *      write-unsupported MFT geometry noted;
 *   2. $MFT bootstrap from record 0, through its attribute list when the
 *      $MFT is fragmented: every extent record must lie inside the part of
 *      $MFT already described, sizes consistent, the
 *      list itself bounded and validated;
 *   3. $MFTMirr: the first min(16, mirror) records compared with $MFT;
 *   4. $Volume: version 1.x / 3.x, flags, label;
 *   5. $AttrDef, $UpCase, the root directory;
 *   6. $Bitmap (size vs. clusters), $LogFile restart pages (32-bit
 *      arithmetic throughout), hibernation, $Secure, $Extend, $BadClus:$Bad;
 *   7. the read-only gate (ntfs.h section 3).
 * =========================================================================== */

/* ---- options ---- */

static void ntfs_opts_default(ntfs_opts_t *o)
{
	mm_memset(o, 0, sizeof(*o));
	o->fmask = NTFS_DEFAULT_FMASK;
	o->dmask = NTFS_DEFAULT_DMASK;
	o->native_symlink = NTFS_SYMLINK_RAW;
	o->streams = NTFS_STREAMS_AUTO;
	/* Corruption met while writing stops writing unless the mount asks
	 * for errors=continue. */
	o->errors = NTFS_ERRORS_RO;
	o->mft_zone_mult = 1;
	o->prealloc = 64 * 1024;
}

/* Parse `data' over `o'.  ST_OK or ST_INVALID (then `o' is unchanged). */
static int ntfs_parse_options(const char *data, ntfs_opts_t *o)
{
	ntfs_opts_t n = *o;
	const char *cur = data, *key, *val;
	char tmp[VFS_MOUNT_OPTS_MAX];
	uint64_t v, umask = 0;
	int have_umask = 0, have_fmask = 0, have_dmask = 0, b;

	while (data && vfs_next_option(&cur, tmp, sizeof(tmp), &key, &val)) {
		if (ntfs_streq(key, "uid")) {
			if (!ntfs_opt_num(val, 10, &v) || v > 0xFFFFFFFFULL)
				return ST_INVALID;
			n.uid = (uint32_t)v;
			n.uid_set = 1;
		} else if (ntfs_streq(key, "gid")) {
			if (!ntfs_opt_num(val, 10, &v) || v > 0xFFFFFFFFULL)
				return ST_INVALID;
			n.gid = (uint32_t)v;
			n.gid_set = 1;
		} else if (ntfs_streq(key, "umask")) {
			if (!ntfs_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			umask = v;
			have_umask = 1;
		} else if (ntfs_streq(key, "fmask")) {
			if (!ntfs_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.fmask = (uint32_t)v;
			have_fmask = 1;
		} else if (ntfs_streq(key, "dmask")) {
			if (!ntfs_opt_num(val, 8, &v) || v > 0777)
				return ST_INVALID;
			n.dmask = (uint32_t)v;
			have_dmask = 1;
		} else if (ntfs_streq(key, "force")) {
			if (!ntfs_opt_bool(val, &n.force))
				return ST_INVALID;
		} else if (ntfs_streq(key, "recover")) {
			if (!ntfs_opt_bool(val, &n.recover))
				return ST_INVALID;
		} else if (ntfs_streq(key, "recovery")) {
			if (!val || !ntfs_streq(val, "ask"))
				return ST_INVALID;
			n.ask_recover = 1;
		} else if (ntfs_streq(key, "discard")) {
			if (!ntfs_opt_bool(val, &n.discard))
				return ST_INVALID;
		} else if (ntfs_streq(key, "nodiscard")) {
			n.discard = 0;
		} else if (ntfs_streq(key, "acl")) {
			if (!ntfs_opt_bool(val, &n.acl))
				return ST_INVALID;
		} else if (ntfs_streq(key, "noacl")) {
			n.acl = 0;
		} else if (ntfs_streq(key, "nocase")) {
			if (!ntfs_opt_bool(val, &n.nocase))
				return ST_INVALID;
		} else if (ntfs_streq(key, "case_sensitive")) {
			if (!ntfs_opt_bool(val, &b))
				return ST_INVALID;
			n.nocase = !b;
		} else if (ntfs_streq(key, "sparse")) {
			if (!ntfs_opt_bool(val, &n.sparse))
				return ST_INVALID;
		} else if (ntfs_streq(key, "disable_sparse")) {
			if (!ntfs_opt_bool(val, &b))
				return ST_INVALID;
			if (b)
				n.sparse = 0;
		} else if (ntfs_streq(key, "show_sys_files") ||
			   ntfs_streq(key, "showmeta")) {
			if (!ntfs_opt_bool(val, &n.show_sys))
				return ST_INVALID;
		} else if (ntfs_streq(key, "hide_dot_files")) {
			if (!ntfs_opt_bool(val, &n.hide_dot))
				return ST_INVALID;
		} else if (ntfs_streq(key, "nohidden")) {
			if (!ntfs_opt_bool(val, &n.nohidden))
				return ST_INVALID;
		} else if (ntfs_streq(key, "sys_immutable")) {
			if (!ntfs_opt_bool(val, &n.sys_immutable))
				return ST_INVALID;
		} else if (ntfs_streq(key, "windows_names")) {
			if (!ntfs_opt_bool(val, &n.windows_names))
				return ST_INVALID;
		} else if (ntfs_streq(key, "native_symlink")) {
			if (ntfs_streq(val, "raw"))
				n.native_symlink = NTFS_SYMLINK_RAW;
			else if (ntfs_streq(val, "rel"))
				n.native_symlink = NTFS_SYMLINK_REL;
			else
				return ST_INVALID;
		} else if (ntfs_streq(key, "symlink")) {
			if (ntfs_streq(val, "wsl"))
				n.symlink_native = 0;
			else if (ntfs_streq(val, "native"))
				n.symlink_native = 1;
			else
				return ST_INVALID;
		} else if (ntfs_streq(key, "streams_interface")) {
			if (ntfs_streq(val, "auto"))
				n.streams = NTFS_STREAMS_AUTO;
			else if (ntfs_streq(val, "windows"))
				n.streams = NTFS_STREAMS_WINDOWS;
			else if (ntfs_streq(val, "none"))
				n.streams = NTFS_STREAMS_NONE;
			else
				return ST_INVALID;
		} else if (ntfs_streq(key, "errors")) {
			if (ntfs_streq(val, "continue"))
				n.errors = NTFS_ERRORS_CONTINUE;
			else if (ntfs_streq(val, "remount-ro"))
				n.errors = NTFS_ERRORS_RO;
			else if (ntfs_streq(val, "panic"))
				n.errors = NTFS_ERRORS_PANIC;
			else
				return ST_INVALID;
		} else if (ntfs_streq(key, "mft_zone_multiplier")) {
			if (!ntfs_opt_num(val, 10, &v) || v < 1 || v > 4)
				return ST_INVALID;
			n.mft_zone_mult = (int)v;
		} else if (ntfs_streq(key, "preallocated_size")) {
			if (!ntfs_opt_num(val, 10, &v) || v > (1ULL << 40))
				return ST_INVALID;
			n.prealloc = v;
		} else if (ntfs_streq(key, "iocharset") || ntfs_streq(key, "nls")) {
			/* Names are UTF-8, always. */
			if (!val || (!ntfs_streq(val, "utf8") &&
				     !ntfs_streq(val, "utf-8")))
				return ST_INVALID;
		}
		/* Anything else (the generic flags: ro, rw, nosuid, nodev,
		 * noexec, noatime, ...) is the VFS's and changes nothing
		 * here. */
	}
	if (have_umask) {
		if (!have_fmask)
			n.fmask = (uint32_t)umask;
		if (!have_dmask)
			n.dmask = (uint32_t)umask;
	}
	*o = n;
	return ST_OK;
}

/* ---- boot sector ---- */

/* Read the boot sector of `bdev' and recognise it.  One bounded read. */
static int ntfs_probe_boot(struct block_device *bdev)
{
	const block_device_t *b = bdev;
	unsigned long ss;
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
	if (b->read((block_device_t *)b, 0, 1, buf) == ST_OK)
		ok = ntfs_boot_sane(buf);
	kfree(buf);
	return ok;
}

static int ntfs_boot_geometry(ntfs_fs_t *fs, const uint8_t *b)
{
	const ntfs_boot_sector_t *bs = (const ntfs_boot_sector_t *)b;
	uint64_t cluster, rec, ib, nsec, spc, vol_bytes, dev_bytes;
	const char *why = NULL;

	if (!ntfs_boot_sane(b)) {
		why = "not an NTFS boot sector";
		goto bad;
	}
	fs->sector_size = bs->bytes_per_sector;
	cluster = ntfs_boot_cluster(fs->sector_size, bs->sectors_per_cluster);
	if (!ntfs_is_pow2(cluster) || cluster < fs->sector_size ||
	    cluster > (2ULL << 20)) {
		why = "bad cluster size";
		goto bad;
	}
	/* Every LBA is in device sectors.  Reads work
	 * in whole clusters (and MFT records through the page cache), so a
	 * cluster must be whole device sectors.  A volume formatted with
	 * NTFS sectors smaller than the device's block (a 512-byte-sector
	 * volume copied to a 4 KB-sector disk) is readable, but its 512-byte
	 * structures cannot be written in place: such a geometry is mounted
	 * read-only (reading is safe). */
	if (cluster < fs->dev_ss) {
		why = "cluster smaller than the device's logical block";
		goto bad;
	}
	if (fs->sector_size < fs->dev_ss || fs->sector_size % fs->dev_ss)
		fs->geometry_ro = 1;
	fs->cluster_size = (uint32_t)cluster;
	fs->cluster_bits = ntfs_log2(cluster);
	spc = cluster / fs->sector_size;
	nsec = bs->number_of_sectors;
	fs->nr_clusters = nsec / spc;
	if (fs->nr_clusters == 0 || fs->nr_clusters > (1ULL << 48)) {
		why = "bad cluster count";
		goto bad;
	}
	if (!ntfs_mul_ok(fs->nr_clusters, cluster, &vol_bytes) ||
	    !ntfs_mul_ok(fs->dev_sectors, fs->dev_ss, &dev_bytes) ||
	    vol_bytes > dev_bytes) {
		why = "the volume is larger than the device";
		goto bad;
	}
	fs->mft_lcn = bs->mft_lcn;
	fs->mftmirr_lcn = bs->mftmirr_lcn;
	if (fs->mft_lcn >= fs->nr_clusters ||
	    fs->mftmirr_lcn >= fs->nr_clusters) {
		why = "$MFT or $MFTMirr outside the volume";
		goto bad;
	}
	rec = ntfs_rec_size(bs->clusters_per_mft_record, fs->cluster_size);
	if (!ntfs_is_pow2(rec) || rec < NTFS_BLOCK_SIZE || rec > PAGE_SIZE) {
		why = "unsupported MFT record size";
		goto bad;
	}
	fs->rec_size = (uint32_t)rec;
	fs->rec_bits = ntfs_log2(rec);
	ib = ntfs_rec_size(bs->clusters_per_index_record, fs->cluster_size);
	if (!ntfs_is_pow2(ib) || ib < NTFS_BLOCK_SIZE || ib > 65536) {
		why = "unsupported index record size";
		goto bad;
	}
	fs->ib_size = (uint32_t)ib;
	fs->serial = bs->volume_serial_number;
	fs->exp_bs = fs->cluster_size < NTFS_MAX_EXPOSED_BLOCK ?
			     fs->cluster_size :
			     (uint32_t)NTFS_MAX_EXPOSED_BLOCK;
	fs->exp_bits = ntfs_log2(fs->exp_bs);
	/* The I/O unit of MFT writes; geometries writes cannot handle
	 * stay read-only (the gate reads geometry_ro). */
	fs->io_unit = fs->dev_ss > fs->rec_size ? fs->dev_ss : fs->rec_size;
	if (fs->rec_size > 2 * fs->cluster_size ||
	    (fs->io_unit > fs->rec_size &&
	     (fs->cluster_size < fs->io_unit ||
	      fs->cluster_size % fs->io_unit)))
		fs->geometry_ro = 1;
	return ST_OK;
bad:
	kprintf("ntfs: %s: %s\n", fs->bdev->name ? fs->bdev->name : "?", why);
	return ST_NOT_FOUND;
}

/* ---- $MFT bootstrap ---- */

static int ntfs_mft_bootstrap(ntfs_fs_t *fs)
{
	ntfs_runlist_t rl0, rl;
	uint8_t *rec0 = NULL, *ext = NULL, *al = NULL;
	ntfs_attr_record_t *a;
	ntfs_stream_t *st = NULL;
	uint32_t al_len = 0;
	uint64_t data_size = 0, init_size = 0, alloc_size = 0;
	int r, have_first = 0;
	const char *why = "corrupt $MFT record 0";

	mm_memset(&rl0, 0, sizeof(rl0));
	mm_memset(&rl, 0, sizeof(rl));
	rec0 = (uint8_t *)kalloc(fs->rec_size);
	ext = (uint8_t *)kalloc(fs->rec_size);
	if (!rec0 || !ext) {
		r = ST_NOMEM;
		goto out;
	}
	/* Record 0 sits at the start of $MFT's first run. */
	r = ntfs_rl_append(&rl0, 0, (int64_t)fs->mft_lcn,
			   (int64_t)((fs->rec_size + fs->cluster_size - 1) >>
				     fs->cluster_bits));
	if (r != ST_OK)
		goto out;
	if (rl0.end_vcn + (int64_t)fs->mft_lcn > (int64_t)fs->nr_clusters) {
		r = ST_IO;
		goto out;
	}
	fs->mft_records = 1; /* for the validators, until the real count */
	r = ntfs_mft_read_raw(fs, &rl0, 0, rec0);
	if (r != ST_OK)
		goto out;
	if (!(((ntfs_mft_record_t *)rec0)->flags & NTFS_MFT_REC_IN_USE) ||
	    ((ntfs_mft_record_t *)rec0)->base_mft_record != 0) {
		r = ST_IO;
		goto out;
	}
	/* The attribute list, when $MFT is fragmented beyond one record. */
	a = ntfs_rec_attr(fs, rec0, 0, NTFS_AT_ATTRIBUTE_LIST, 0, 0, 0);
	if (a) {
		if (!a->non_resident) {
			al_len = a->data.resident.value_length;
			al = (uint8_t *)kalloc(al_len ? al_len : 1);
			if (!al) {
				r = ST_NOMEM;
				goto out;
			}
			mm_memcpy(al, ntfs_attr_value(a), al_len);
		} else {
			ntfs_runlist_t arl;
			uint64_t ds = (uint64_t)a->data.non_resident.data_size;

			/* The 64-bit size is checked first. */
			if (ds > NTFS_ATTR_LIST_MAX) {
				why = "$MFT attribute list too large";
				r = ST_IO;
				goto out;
			}
			mm_memset(&arl, 0, sizeof(arl));
			r = ntfs_mp_decode(fs, a, a->length, &arl);
			if (r == ST_OK && ((uint64_t)arl.end_vcn
					   << fs->cluster_bits) < ds)
				r = ST_IO;
			if (r == ST_OK) {
				al_len = (uint32_t)ds;
				al = (uint8_t *)kalloc(al_len ? al_len : 1);
				r = al ? ntfs_rl_pread(fs, &arl, 0, ds,
						       (uint64_t)a->data
							       .non_resident
							       .initialized_size,
						       al) :
					 ST_NOMEM;
			}
			ntfs_rl_free(&arl);
			if (r != ST_OK)
				goto out;
		}
		fs->mft_records = 0; /* skip the range test of entries */
		r = ntfs_al_valid(fs, al, al_len, 0);
		fs->mft_records = 1;
		if (r != ST_OK) {
			why = "corrupt $MFT attribute list";
			goto out;
		}
	}
	/* Collect the unnamed $DATA extents in VCN order. */
	if (!al) {
		a = ntfs_rec_attr(fs, rec0, 0, NTFS_AT_DATA, 0, 0, 1);
		if (!a || !a->non_resident ||
		    a->data.non_resident.lowest_vcn != 0) {
			r = ST_IO;
			goto out;
		}
		data_size = (uint64_t)a->data.non_resident.data_size;
		init_size = (uint64_t)a->data.non_resident.initialized_size;
		alloc_size = (uint64_t)a->data.non_resident.allocated_size;
		have_first = 1;
		if (a->flags & (NTFS_ATTR_COMPRESSION_MASK |
				NTFS_ATTR_IS_ENCRYPTED | NTFS_ATTR_IS_SPARSE)) {
			r = ST_IO;
			goto out;
		}
		r = ntfs_mp_decode(fs, a, a->length, &rl);
		if (r != ST_OK)
			goto out;
	} else {
		for (uint32_t pos = 0; pos < al_len;) {
			const ntfs_attr_list_entry_t *e =
				(const ntfs_attr_list_entry_t *)(al + pos);
			uint64_t eno = NTFS_MREF_NO(e->mft_reference);
			uint8_t *rec;

			pos += e->length;
			if (e->type != NTFS_AT_DATA || e->name_length)
				continue;
			if (eno == 0) {
				rec = rec0;
			} else {
				uint64_t off;

				/* The extent record must be readable
				 * through what is described already. */
				if (!ntfs_mul_ok(eno, fs->rec_size, &off) ||
				    off + fs->rec_size >
					    ((uint64_t)rl.end_vcn
					     << fs->cluster_bits)) {
					why = "$MFT needs its own extent records "
					      "to describe itself";
					r = ST_IO;
					goto out;
				}
				r = ntfs_mft_read_raw(fs, &rl, eno, ext);
				if (r != ST_OK)
					goto out;
				if (!(((ntfs_mft_record_t *)ext)->flags &
				      NTFS_MFT_REC_IN_USE) ||
				    NTFS_MREF_NO(((ntfs_mft_record_t *)ext)
							 ->base_mft_record) != 0) {
					r = ST_IO;
					goto out;
				}
				rec = ext;
			}
			a = ntfs_rec_attr(fs, rec, eno, NTFS_AT_DATA, 1,
					  e->instance, 1);
			if (!a || !a->non_resident ||
			    a->data.non_resident.lowest_vcn != e->lowest_vcn) {
				r = ST_IO;
				goto out;
			}
			if (a->data.non_resident.lowest_vcn == 0) {
				if (have_first) {
					r = ST_IO;
					goto out;
				}
				have_first = 1;
				data_size = (uint64_t)
					a->data.non_resident.data_size;
				init_size = (uint64_t)
					a->data.non_resident.initialized_size;
				alloc_size = (uint64_t)
					a->data.non_resident.allocated_size;
				if (a->flags & (NTFS_ATTR_COMPRESSION_MASK |
						NTFS_ATTR_IS_ENCRYPTED |
						NTFS_ATTR_IS_SPARSE)) {
					r = ST_IO;
					goto out;
				}
			} else if (!have_first) {
				r = ST_IO;
				goto out;
			}
			r = ntfs_mp_decode(fs, a, a->length, &rl);
			if (r != ST_OK)
				goto out;
		}
		if (!have_first) {
			r = ST_IO;
			goto out;
		}
	}
	/* Sizes against the allocation, and the runlist must
	 * describe the whole data. */
	if (data_size > alloc_size || init_size > data_size ||
	    ((uint64_t)rl.end_vcn << fs->cluster_bits) < data_size ||
	    rl.count == 0 || rl.runs[0].lcn != (int64_t)fs->mft_lcn) {
		why = "$MFT sizes or runlist inconsistent";
		r = ST_IO;
		goto out;
	}
	for (uint32_t i = 0; i < rl.count; i++)
		if (rl.runs[i].lcn == NTFS_LCN_HOLE) {
			why = "$MFT is sparse";
			r = ST_IO;
			goto out;
		}
	fs->mft_records = data_size >> fs->rec_bits;
	if (fs->mft_records < NTFS_FILE_FIRST_USER ||
	    fs->mft_records > NTFS_MAX_MFT_NO) {
		why = "bad $MFT record count";
		r = ST_IO;
		goto out;
	}
	st = (ntfs_stream_t *)ntfs_zalloc(sizeof(*st));
	if (!st) {
		r = ST_NOMEM;
		goto out;
	}
	st->mft_no = NTFS_FILE_MFT;
	st->seq = ((ntfs_mft_record_t *)rec0)->sequence_number;
	st->type = NTFS_AT_DATA;
	st->data_size = data_size;
	st->init_size = init_size;
	st->alloc_size = alloc_size;
	st->rl = rl;
	mm_memset(&rl, 0, sizeof(rl));
	ntfs_stream_install_reserved(fs, st, NTFS_HANDLE_MFT);
	fs->st_mft = st;
	/* New records get record 0's update sequence layout. */
	fs->new_usa_ofs = ((ntfs_mft_record_t *)rec0)->usa_ofs;
	r = ST_OK;
out:
	if (r != ST_OK)
		kprintf("ntfs: %s: %s\n", fs->bdev->name ? fs->bdev->name : "?",
			why);
	ntfs_rl_free(&rl0);
	ntfs_rl_free(&rl);
	ntfs_free(rec0);
	ntfs_free(ext);
	ntfs_free(al);
	return r;
}

/* ---- $MFTMirr ---- */

static int ntfs_check_mirror(ntfs_fs_t *fs)
{
	ntfs_runlist_t mrl;
	ntfs_attr_sizes_t sz;
	uint8_t *a = NULL, *b = NULL;
	uint64_t mirr_recs, n;
	int r;

	r = ntfs_attr_runlist(fs, NTFS_MAKE_MREF(NTFS_FILE_MFTMIRR, 0),
			      NTFS_AT_DATA, ntfs_unnamed, 0, &mrl, &sz);
	if (r != ST_OK) {
		kprintf("ntfs: %s: cannot read $MFTMirr\n", fs->bdev->name);
		return ST_IO;
	}
	mirr_recs = sz.data_size >> fs->rec_bits;
	if (sz.resident || mirr_recs < 4 || mrl.count == 0 ||
	    mrl.runs[0].lcn != (int64_t)fs->mftmirr_lcn) {
		kprintf("ntfs: %s: $MFTMirr is invalid\n", fs->bdev->name);
		ntfs_rl_free(&mrl);
		return ST_IO;
	}
	n = mirr_recs < NTFS_MIRR_CMP_MAX ? mirr_recs : NTFS_MIRR_CMP_MAX;
	a = (uint8_t *)kalloc(fs->rec_size);
	b = (uint8_t *)kalloc(fs->rec_size);
	if (!a || !b) {
		r = ST_NOMEM;
		goto out;
	}
	for (uint64_t i = 0; i < n; i++) {
		int ra, rb;
		uint32_t used;

		ra = ntfs_rl_pread(fs, &fs->st_mft->rl, i << fs->rec_bits,
				   fs->rec_size, ~0ULL, a);
		rb = ntfs_rl_pread(fs, &mrl, i << fs->rec_bits, fs->rec_size,
				   sz.init_size, b);
		if (ra != ST_OK || rb != ST_OK) {
			r = ST_IO;
			goto out;
		}
		ra = ntfs_mft_rec_check(fs, a, i);
		rb = ntfs_mft_rec_check(fs, b, i);
		if (ra == ST_NOT_FOUND && rb == ST_NOT_FOUND)
			continue; /* both never initialised */
		used = ((ntfs_mft_record_t *)a)->bytes_in_use;
		if (ra != ST_OK || rb != ST_OK ||
		    used != ((ntfs_mft_record_t *)b)->bytes_in_use ||
		    kmemcmp(a, b, used) != 0) {
			kprintf("ntfs: %s: $MFT and $MFTMirr record %llu do not "
				"match.  Run chkdsk.\n",
				fs->bdev->name, (unsigned long long)i);
			r = ST_IO;
			goto out;
		}
	}
	r = ST_OK;
	/* Kept for the mirror writes (S23): every record it holds. */
	fs->mirr_recs = mirr_recs;
	fs->mirr_rl = mrl;
	mm_memset(&mrl, 0, sizeof(mrl));
out:
	ntfs_free(a);
	ntfs_free(b);
	ntfs_rl_free(&mrl);
	return r;
}

/* ---- $Volume ---- */

static int ntfs_load_volume(ntfs_fs_t *fs)
{
	ntfs_actx_t *c;
	int r;

	r = ntfs_actx_open(fs, NTFS_MAKE_MREF(NTFS_FILE_VOLUME, 0), &c);
	if (r != ST_OK)
		return ST_IO;
	r = ntfs_actx_find(c, NTFS_AT_VOLUME_NAME, NULL, 0, 0);
	if (r == ST_OK) {
		const ntfs_attr_record_t *a = c->a;
		uint32_t n = a->data.resident.value_length / 2;

		fs->label = (char *)kalloc(n * 3 + 1);
		if (fs->label &&
		    ntfs_utf16_path((const uint16_t *)ntfs_attr_value(a), n,
				    fs->label, n * 3 + 1) < 0)
			fs->label[0] = '\0';
	} else if (r == ST_IO) {
		kprintf("ntfs: %s: corrupt volume label ignored\n",
			fs->bdev->name);
	}
	/* A fresh walk, whatever the label lookup returned. */
	ntfs_actx_reset(c);
	r = ntfs_actx_find(c, NTFS_AT_VOLUME_INFORMATION, NULL, 0, 0);
	if (r == ST_OK) {
		const ntfs_volume_information_t *vi =
			(const ntfs_volume_information_t *)ntfs_attr_value(c->a);

		fs->major_ver = vi->major_ver;
		fs->minor_ver = vi->minor_ver;
		fs->vol_flags = vi->flags;
		/* Never cleared by this mount (a forced mount of a dirty
		 * volume leaves it dirty). */
		fs->was_dirty = (vi->flags & NTFS_VOLUME_IS_DIRTY) != 0;
	}
	ntfs_actx_close(c);
	if (r != ST_OK) {
		kprintf("ntfs: %s: no $VOLUME_INFORMATION\n", fs->bdev->name);
		return ST_IO;
	}
	if (!fs->label) {
		fs->label = (char *)kalloc(1);
		if (!fs->label)
			return ST_NOMEM;
		fs->label[0] = '\0';
	}
	if (!((fs->major_ver == 1 && fs->minor_ver <= 2) ||
	      (fs->major_ver == 3 && fs->minor_ver <= 1))) {
		kprintf("ntfs: %s: unsupported NTFS version %u.%u\n",
			fs->bdev->name, fs->major_ver, fs->minor_ver);
		return ST_UNSUPPORTED;
	}
	return ST_OK;
}

/* ---- $AttrDef, $UpCase ---- */

static int ntfs_load_attrdef_upcase(ntfs_fs_t *fs)
{
	uint8_t *buf = NULL;
	uint32_t len = 0;
	int r;

	r = ntfs_attr_readall(fs, NTFS_MAKE_MREF(NTFS_FILE_ATTRDEF, 0),
			      NTFS_AT_DATA, ntfs_unnamed, 0, NTFS_ATTRDEF_MAX,
			      &buf, &len);
	if (r != ST_OK || len < NTFS_ATTRDEF_ENTRY) {
		/* A table shorter than one entry is refused. */
		kprintf("ntfs: %s: cannot load $AttrDef\n", fs->bdev->name);
		ntfs_free(buf);
		return ST_IO;
	}
	fs->attrdef = buf;
	fs->attrdef_len = len - len % NTFS_ATTRDEF_ENTRY;
	buf = NULL;
	r = ntfs_attr_readall(fs, NTFS_MAKE_MREF(NTFS_FILE_UPCASE, 0),
			      NTFS_AT_DATA, ntfs_unnamed, 0,
			      NTFS_UPCASE_MAX * 2, &buf, &len);
	if (r != ST_OK || len < 256 || (len & 1)) {
		kprintf("ntfs: %s: cannot load $UpCase\n", fs->bdev->name);
		ntfs_free(buf);
		return ST_IO;
	}
	/* The volume's own table, whatever its length. */
	fs->upcase_len = len / 2;
	fs->upcase = (uint16_t *)kalloc(len);
	if (!fs->upcase) {
		kfree(buf);
		return ST_NOMEM;
	}
	for (uint32_t i = 0; i < fs->upcase_len; i++)
		fs->upcase[i] = ntfs_le16(buf + 2 * i);
	kfree(buf);
	return ST_OK;
}

/* ---- $LogFile restart pages (state for the rw gate) ---- */

static void ntfs_check_logfile(ntfs_fs_t *fs)
{
	ntfs_stream_t *st = fs->st_logfile;
	uint8_t *buf = NULL;
	uint64_t want;
	int clean1 = 0, clean2 = 0, r1, r2 = ST_NOT_FOUND, all_ff = 1;
	int64_t lsn1 = 0, lsn2 = 0;
	uint32_t ps1 = 0, ps2 = 0;

	fs->log_state = NTFS_LOG_BAD;
	if (!st || (st->flags & NTFS_ST_RESIDENT) || st->data_size < 2 * 4096)
		return;
	want = ntfs_min64(st->data_size, 2 * 65536ULL);
	buf = (uint8_t *)kalloc(want);
	if (!buf)
		return;
	if (ntfs_rl_pread(fs, &st->rl, 0, want, st->init_size, buf) != ST_OK)
		goto out;
	for (uint64_t i = 0; i < 4096 && all_ff; i++)
		all_ff = buf[i] == 0xFF;
	if (all_ff) {
		/* Emptied by an earlier read-write mount: no log to replay;
		 * the system that next writes the volume initialises it. */
		fs->log_state = NTFS_LOG_EMPTY;
		goto out;
	}
	r1 = ntfs_rstr_check(buf, (uint32_t)want, &ps1, &clean1, &lsn1);
	if (r1 == ST_OK && ps1 < want)
		r2 = ntfs_rstr_check(buf + ps1, (uint32_t)(want - ps1), &ps2,
				     &clean2, &lsn2);
	if (r1 != ST_OK && r2 != ST_OK) {
		/* The first page damaged: find the second copy at the
		 * usual system page sizes. */
		for (uint32_t off = 512; off <= 65536 && off < want; off <<= 1) {
			r2 = ntfs_rstr_check(buf + off, (uint32_t)(want - off),
					     &ps2, &clean2, &lsn2);
			if (r2 == ST_OK && ps2 == off)
				break;
			r2 = ST_NOT_FOUND;
		}
	}
	if (r1 != ST_OK && r2 != ST_OK)
		goto out; /* BAD */
	/* The newer of the two copies decides. */
	if (r1 == ST_OK && (r2 != ST_OK || lsn1 >= lsn2)) {
		fs->log_state = clean1 ? NTFS_LOG_CLEAN : NTFS_LOG_UNCLEAN;
		fs->log_current_lsn = lsn1;
	} else {
		fs->log_state = clean2 ? NTFS_LOG_CLEAN : NTFS_LOG_UNCLEAN;
		fs->log_current_lsn = lsn2;
	}
out:
	kfree(buf);
}

/* ---- hibernation ---- */

/* 1: hibernated or undeterminable, 0: not hibernated. */
static int ntfs_check_hibernation(ntfs_fs_t *fs)
{
	static const uint16_t hib[12] = { 'h', 'i', 'b', 'e', 'r', 'f',
					  'i', 'l', '.', 's', 'y', 's' };
	ntfs_index_t *ix = NULL;
	ntfs_runlist_t rl;
	ntfs_attr_sizes_t sz;
	uint8_t *ent = NULL, *buf = NULL;
	ntfs_ikey_t k;
	uint64_t mref;
	int r, hibernated = 1, zero = 1;

	r = ntfs_index_open(fs, NTFS_MAKE_MREF(NTFS_FILE_ROOT, 0),
			    ntfs_i30_name, 4, NULL, &ix);
	if (r != ST_OK)
		return 1;
	k.key = hib;
	k.len = 12;
	k.ic = 1; /* the name is case-insensitive on that system */
	r = ntfs_index_lookup(ix, &k, &ent);
	ntfs_index_close(ix);
	if (r == ST_NOT_FOUND)
		return 0;
	if (r != ST_OK)
		return 1;
	mref = ((const ntfs_index_entry_t *)ent)->data.indexed_file;
	kfree(ent);
	r = ntfs_attr_runlist(fs, mref, NTFS_AT_DATA, ntfs_unnamed, 0, &rl,
			      &sz);
	if (r == ST_NOT_FOUND)
		return 1; /* no data: smaller than a page */
	if (r != ST_OK)
		return 1;
	if (sz.resident || sz.data_size < 4096 || sz.init_size < 4096 ||
	    (sz.flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_IS_ENCRYPTED)))
		goto out; /* hibernated (smaller than 4 KB) or unreadable */
	buf = (uint8_t *)kalloc(4096);
	if (!buf || ntfs_rl_pread(fs, &rl, 0, 4096, sz.init_size, buf) != ST_OK)
		goto out;
	for (int i = 0; i < 4096 && zero; i++)
		zero = buf[i] == 0;
	if (zero)
		hibernated = 0;
	/* "hibr" / "HIBR" / "wake" / "WAKE" and anything else that is not
	 * zero: hibernated (Fast Startup included). */
out:
	ntfs_free(buf);
	ntfs_rl_free(&rl);
	return hibernated;
}

/* ---- $BadClus:$Bad ---- */

static int ntfs_check_badclus(ntfs_fs_t *fs)
{
	static const uint16_t bad[4] = { '$', 'B', 'a', 'd' };
	ntfs_runlist_t rl;
	ntfs_attr_sizes_t sz;
	int r;

	r = ntfs_attr_runlist(fs, NTFS_MAKE_MREF(NTFS_FILE_BADCLUS, 0),
			      NTFS_AT_DATA, bad, 4, &rl, &sz);
	if (r != ST_OK)
		return r;
	r = ST_OK;
	if (sz.resident || (uint64_t)rl.end_vcn < fs->nr_clusters)
		r = ST_IO;
	for (uint32_t i = 0; r == ST_OK && i < rl.count; i++)
		if (rl.runs[i].lcn != NTFS_LCN_HOLE &&
		    (rl.runs[i].lcn != rl.runs[i].vcn ||
		     (uint64_t)(rl.runs[i].lcn + rl.runs[i].len) >
			     fs->nr_clusters))
			r = ST_IO;
	if (r == ST_OK) {
		ntfs_rl_free(&fs->bad_rl);
		fs->bad_rl = rl; /* kept: reserved before the first allocation */
		return ST_OK;
	}
	ntfs_rl_free(&rl);
	return r;
}

/* ---- instance lifetime ---- */

static void ntfs_fs_release(ntfs_fs_t *fs)
{
	if (!fs)
		return;
	if (fs->sb.ops)
		vfs_sb_release_caches(&fs->sb);
	ntfs_streams_release(fs);
	ntfs_rc_release(fs);
	ntfs_ic_release(fs);
	ntfs_rl_free(&fs->mirr_rl);
	ntfs_rl_free(&fs->bad_rl);
	for (int i = 0; i < 64; i++) {
		while (fs->ofiles[i]) {
			struct ntfs_ofile *o = fs->ofiles[i];

			fs->ofiles[i] = o->next;
			kfree(o);
		}
	}
	ntfs_free(fs->upcase);
	ntfs_free(fs->attrdef);
	ntfs_free(fs->label);
	fs->upcase = NULL;
	fs->attrdef = NULL;
	fs->label = NULL;
}

static int ntfs_fs_init(ntfs_fs_t *fs, block_device_t *bdev)
{
	fs->bdev = bdev;
	fs->dev_ss = bdev->sector_size ? bdev->sector_size : 512;
	fs->dev_sectors = bdev->total_sectors;
	if (!ntfs_is_pow2(fs->dev_ss) || fs->dev_ss < 512 ||
	    fs->dev_ss > NTFS_MAX_EXPOSED_BLOCK || fs->dev_sectors < 2)
		return ST_INVALID;
	fs->dev_ss_bits = ntfs_log2(fs->dev_ss);
	ntfs_rwsem_init(&fs->meta, "ntfs_meta");
	ntfs_rwsem_init(&fs->alloc, "ntfs_alloc");
	for (int i = 0; i < NTFS_FLOCKS; i++)
		ntfs_rwsem_init(&fs->flock[i], "ntfs_file");
	spinlock_init(&fs->hdl_lock, "ntfs_hdl");
	spinlock_init(&fs->sf_lock, "ntfs_statfs");
	ntfs_rc_init(fs);
	ntfs_ic_init(fs);
	fs->lru.lru_next = fs->lru.lru_prev = &fs->lru;
	fs->slot_hint = NTFS_HANDLE_FIRST_DYNAMIC;
	fs->slots = (ntfs_stream_t **)ntfs_zalloc(NTFS_SLOTS *
						  sizeof(ntfs_stream_t *));
	if (!fs->slots)
		return ST_NOMEM;
	ntfs_guard_init(fs);
	fs->sb.ops = &ntfs_sb_ops;
	fs->sb.fs_private = fs;
	fs->cwd = NTFS_MAKE_MREF(NTFS_FILE_ROOT, 0);
	return ST_OK;
}

/* ---- the read-write gate ----
 *
 * Every reason for staying read-only is a bit of fs->gate; only
 * NTFS_GATE_DIRTY may be overridden (by `force'), at mount and at a
 * remount read-write alike.  fs->ro_reason names the first reason. */
#define NTFS_GATE_HIBER 0x001U
#define NTFS_GATE_ERRORS 0x002U
#define NTFS_GATE_MIRROR 0x004U
#define NTFS_GATE_DIRTY 0x008U
#define NTFS_GATE_LOG 0x010U
#define NTFS_GATE_GEOMETRY 0x020U
#define NTFS_GATE_VERSION 0x040U
#define NTFS_GATE_DEVRO 0x080U
#define NTFS_GATE_BITMAP 0x100U
#define NTFS_GATE_UNSUPPORTED 0x200U
#define NTFS_GATE_RUNTIME 0x400U

static void ntfs_gate_eval(ntfs_fs_t *fs)
{
	uint32_t g = fs->gate & (NTFS_GATE_BITMAP | NTFS_GATE_RUNTIME);

	if (fs->hibernated)
		g |= NTFS_GATE_HIBER;
	if (fs->mirr_mismatch)
		g |= NTFS_GATE_MIRROR;
	if (fs->mount_errors)
		g |= NTFS_GATE_ERRORS;
	if (fs->errors || fs->latched_ro)
		g |= NTFS_GATE_RUNTIME;
	if (fs->vol_flags & NTFS_VOLUME_MUST_MOUNT_RO_MASK)
		g |= NTFS_GATE_DIRTY;
	if (fs->log_state == NTFS_LOG_UNCLEAN || fs->log_state == NTFS_LOG_BAD)
		g |= NTFS_GATE_LOG;
	if (fs->geometry_ro)
		g |= NTFS_GATE_GEOMETRY;
	if (fs->major_ver != 3)
		g |= NTFS_GATE_VERSION;
	if (blk_bdev_read_only(fs->bdev))
		g |= NTFS_GATE_DEVRO;
	if (!NTFS_WRITE_SUPPORTED)
		g |= NTFS_GATE_UNSUPPORTED;
	fs->gate = g;
	if (fs->opt.force)
		g &= ~NTFS_GATE_DIRTY; /* force overrides this one only */
	fs->rw_ok = g == 0;
	if (g & NTFS_GATE_HIBER)
		fs->ro_reason = "the volume is hibernated or Fast Startup is on "
				"(hiberfil.sys); shut the other system down fully "
				"to write";
	else if (g & NTFS_GATE_MIRROR)
		fs->ro_reason = "$MFT and $MFTMirr differ; run chkdsk";
	else if (g & (NTFS_GATE_ERRORS | NTFS_GATE_RUNTIME))
		fs->ro_reason = "errors were found; run chkdsk";
	else if (g & NTFS_GATE_DIRTY)
		fs->ro_reason = "the volume is marked dirty or chkdsk is "
				"pending; run chkdsk or mount with force";
	else if (g & NTFS_GATE_LOG)
		fs->ro_reason = "unclean journal; mount with -o recover to "
				"replay it, or run chkdsk";
	else if (g & NTFS_GATE_GEOMETRY)
		fs->ro_reason = "MFT geometry is supported read-only";
	else if (g & NTFS_GATE_VERSION)
		fs->ro_reason = "only NTFS 3.x volumes are written";
	else if (g & NTFS_GATE_DEVRO)
		fs->ro_reason = "the device is read-only";
	else if (g & NTFS_GATE_BITMAP)
		fs->ro_reason = "system files are not marked in use in $Bitmap; "
				"run chkdsk";
	else if (g & NTFS_GATE_UNSUPPORTED)
		fs->ro_reason = "write support is not available";
	else
		fs->ro_reason = NULL;
}

/* Are all clusters of `rl' marked in use? */
static int ntfs_rl_marked(ntfs_fs_t *fs, const ntfs_runlist_t *rl)
{
	for (uint32_t i = 0; i < rl->count; i++) {
		const ntfs_run_t *x = &rl->runs[i];
		uint64_t c, end;

		if (x->lcn == NTFS_LCN_HOLE)
			continue;
		c = (uint64_t)x->lcn;
		end = c + (uint64_t)x->len;
		while (c < end) {
			uint32_t scanned = 0;
			uint64_t f;
			int r = ntfs_bm_next_free(fs, c, end, &f, 0, &scanned);

			if (r == ST_NOT_FOUND)
				break;
			if (r != ST_OK)
				return r;
			WARN_RATELIMIT(1, "ntfs: cluster %llu of a system file is "
					  "free in $Bitmap",
				       (unsigned long long)f);
			return ST_IO;
		}
	}
	return ST_OK;
}

/* The clusters of attribute (type, name) of system file `no' are in use
 * in $Bitmap (a missing attribute, or a resident one, has none). */
static int ntfs_attr_marked(ntfs_fs_t *fs, uint64_t no, uint32_t type,
			    const uint16_t *name, uint32_t nlen)
{
	ntfs_runlist_t rl;
	ntfs_attr_sizes_t sz;
	int r;

	if (no >= fs->mft_records)
		return ST_OK;
	r = ntfs_attr_runlist(fs, NTFS_MAKE_MREF(no, 0), type,
			      name ? name : ntfs_unnamed, nlen, &rl, &sz);
	if (r == ST_NOT_FOUND)
		return ST_OK;
	if (r != ST_OK)
		return r;
	if (!sz.resident)
		r = ntfs_rl_marked(fs, &rl);
	ntfs_rl_free(&rl);
	return r;
}

/* Before a mount may write: the system files' own clusters ($MFT,
 * $MFTMirr, $LogFile, $Bitmap, $MFT:$BITMAP, the root index) must be in
 * use in $Bitmap -- else an allocation could hand them out. */
static int ntfs_check_system_clusters(ntfs_fs_t *fs)
{
	ntfs_stream_t *root = NULL;
	int r;

	if (!fs->st_bitmap)
		return ST_IO;
	r = ntfs_rl_marked(fs, &fs->st_mft->rl);
	if (r == ST_OK)
		r = ntfs_rl_marked(fs, &fs->mirr_rl);
	if (r == ST_OK && fs->st_logfile)
		r = ntfs_rl_marked(fs, &fs->st_logfile->rl);
	if (r == ST_OK)
		r = ntfs_rl_marked(fs, &fs->st_bitmap->rl);
	if (r == ST_OK && !(fs->st_mftbmp->flags & NTFS_ST_RESIDENT))
		r = ntfs_rl_marked(fs, &fs->st_mftbmp->rl);
	if (r == ST_OK &&
	    ntfs_dir_stream(fs, NTFS_MAKE_MREF(NTFS_FILE_ROOT, 0), &root) ==
		    ST_OK) {
		if (!(root->flags & NTFS_ST_NOALLOC))
			r = ntfs_rl_marked(fs, &root->rl);
		ntfs_stream_put(fs, root);
	}
	/* The other system files with clusters of their own. */
	{
		static const uint16_t sds[4] = { '$', 'S', 'D', 'S' };
		static const uint16_t sii[4] = { '$', 'S', 'I', 'I' };
		static const uint16_t sdh[4] = { '$', 'S', 'D', 'H' };
		static const struct {
			uint64_t no;
			uint32_t type;
			const uint16_t *name;
			uint32_t nlen;
		} sys[] = {
			{ NTFS_FILE_BOOT, NTFS_AT_DATA, NULL, 0 },
			{ NTFS_FILE_ATTRDEF, NTFS_AT_DATA, NULL, 0 },
			{ NTFS_FILE_UPCASE, NTFS_AT_DATA, NULL, 0 },
			{ NTFS_FILE_SECURE, NTFS_AT_DATA, sds, 4 },
			{ NTFS_FILE_SECURE, NTFS_AT_INDEX_ALLOCATION, sii, 4 },
			{ NTFS_FILE_SECURE, NTFS_AT_INDEX_ALLOCATION, sdh, 4 },
			{ NTFS_FILE_EXTEND, NTFS_AT_INDEX_ALLOCATION,
			  ntfs_i30_name, 4 },
		};

		for (uint32_t i = 0; r == ST_OK && i < sizeof(sys) / sizeof(sys[0]);
		     i++)
			r = ntfs_attr_marked(fs, sys[i].no, sys[i].type,
					     sys[i].name, sys[i].nlen);
	}
	return r;
}

/* Load the volume.  ST_OK with fs->ro / fs->rw_ok / fs->ro_reason set. */
static int ntfs_load(ntfs_fs_t *fs, int want_ro)
{
	uint8_t *boot = NULL;
	ntfs_stream_t *root = NULL;
	int r;
	const char *dev = fs->bdev->name ? fs->bdev->name : "?";

	boot = (uint8_t *)kalloc(fs->dev_ss > 512 ? fs->dev_ss : 512);
	if (!boot)
		return ST_NOMEM;
	r = ntfs_dev_read(fs, 0, (fs->dev_ss >= 512) ? 1 : 512 / fs->dev_ss,
			  boot);
	if (r == ST_OK)
		r = ntfs_boot_geometry(fs, boot);
	kfree(boot);
	if (r != ST_OK)
		return r;
	r = ntfs_mft_bootstrap(fs);
	if (r != ST_OK)
		return r == ST_NOMEM ? r : ST_IO;
	r = ntfs_stream_open_reserved(fs, NTFS_FILE_MFT, NTFS_AT_BITMAP, NULL,
				      NTFS_HANDLE_MFT_BITMAP, &fs->st_mftbmp);
	if (r != ST_OK) {
		kprintf("ntfs: %s: cannot open $MFT:$BITMAP\n", dev);
		return ST_IO;
	}
	if (ntfs_check_mirror(fs) != ST_OK) {
		fs->mirr_mismatch = 1;
		fs->mount_errors = 1;
	}
	r = ntfs_load_volume(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_load_attrdef_upcase(fs);
	if (r != ST_OK)
		return r;
	r = ntfs_dir_stream(fs, NTFS_MAKE_MREF(NTFS_FILE_ROOT, 0), &root);
	if (r != ST_OK) {
		kprintf("ntfs: %s: cannot open the root directory\n", dev);
		return ST_IO;
	}
	ntfs_stream_put(fs, root);
	/* The index block geometry new directories get: the root's. */
	{
		uint8_t *ir = NULL;
		uint32_t irl = 0;

		fs->ir_cpib = fs->ib_size >= fs->cluster_size ?
				      (uint8_t)(fs->ib_size / fs->cluster_size) :
				      (uint8_t)(fs->ib_size / NTFS_BLOCK_SIZE);
		if (ntfs_index_root_read(fs, NTFS_MAKE_MREF(NTFS_FILE_ROOT, 0),
					 ntfs_i30_name, 4, &ir, &irl) == ST_OK &&
		    irl >= NTFS_INDEX_ROOT_HDR &&
		    ((ntfs_index_root_t *)ir)->index_block_size == fs->ib_size)
			fs->ir_cpib =
				((ntfs_index_root_t *)ir)->clusters_per_index_block;
		ntfs_free(ir);
	}
	r = ntfs_stream_open_reserved(fs, NTFS_FILE_BITMAP, NTFS_AT_DATA, NULL,
				      NTFS_HANDLE_BITMAP, &fs->st_bitmap);
	if (r != ST_OK || fs->st_bitmap->data_size < (fs->nr_clusters + 7) / 8) {
		kprintf("ntfs: %s: $Bitmap is unusable or too small\n", dev);
		fs->mount_errors = 1;
	}
	r = ntfs_stream_open_reserved(fs, NTFS_FILE_LOGFILE, NTFS_AT_DATA, NULL,
				      NTFS_HANDLE_LOGFILE, &fs->st_logfile);
	if (r == ST_OK) {
		ntfs_check_logfile(fs);
	} else {
		fs->log_state = NTFS_LOG_BAD;
	}
	if (fs->log_state == NTFS_LOG_BAD) {
		kprintf("ntfs: %s: $LogFile is invalid\n", dev);
		fs->mount_errors = 1;
	}
	fs->hibernated = ntfs_check_hibernation(fs);
	if (fs->major_ver >= 3) {
		static const uint16_t sii[4] = { '$', 'S', 'I', 'I' };
		ntfs_index_t *ix = NULL;
		int is_link = 0, is_dir = 0;

		r = ntfs_stream_open_reserved(fs, NTFS_FILE_SECURE,
					      NTFS_AT_DATA, "$SDS",
					      NTFS_HANDLE_SECURE_SDS,
					      &fs->st_sds);
		if (r == ST_OK)
			r = ntfs_index_open(fs,
					    NTFS_MAKE_MREF(NTFS_FILE_SECURE, 0),
					    sii, 4, NULL, &ix);
		ntfs_index_close(ix);
		if (r != ST_OK) {
			kprintf("ntfs: %s: $Secure is unusable\n", dev);
			fs->mount_errors = 1;
		}
		r = ntfs_inode_quick_link(fs, NTFS_MAKE_MREF(NTFS_FILE_EXTEND, 0),
					  &is_link, &is_dir);
		if (r != ST_OK || !is_dir) {
			kprintf("ntfs: %s: $Extend is unusable\n", dev);
			fs->mount_errors = 1;
		}
	}
	fs->badclus_ok = ntfs_check_badclus(fs) == ST_OK;
	if (!fs->badclus_ok) {
		kprintf("ntfs: %s: $BadClus is invalid.  Run chkdsk.\n", dev);
		fs->mount_errors = 1;
	}
	/* The read-only gate (ntfs.h section 3). */
	ntfs_gate_eval(fs);
	/* An unclean $LogFile is replayed only when the mount asks for it
	 * (option `recover'), that is the only reason for read-only and a
	 * read-write mount was asked for; on success the mount starts over
	 * on the replayed volume (fs->replayed).  Without `recover' the
	 * volume stays read-only and nothing is written, as if the driver
	 * had no replay at all: the replay is verified against logs this
	 * project generates, not against every writer's logs, so it is
	 * never done on a mount nobody asked it for (automatic mounts never
	 * pass the option). */
	if (!want_ro && fs->log_state == NTFS_LOG_UNCLEAN && !fs->hibernated &&
	    (fs->gate & ~(uint32_t)NTFS_GATE_LOG) == 0 && !fs->opt.recover &&
	    fs->opt.ask_recover) {
		/* recovery=ask: the mount that would replay it is the one
		 * the caller makes after asking whether to; this one stops. */
		kprintf("ntfs: %s: $LogFile needs to be replayed: not mounted "
			"(the option recover replays it)\n",
			fs->bdev && fs->bdev->name ? fs->bdev->name : "?");
		return ST_UCLEAN;
	}
	if (!want_ro && fs->opt.recover && fs->log_state == NTFS_LOG_UNCLEAN &&
	    !fs->hibernated && (fs->gate & ~(uint32_t)NTFS_GATE_LOG) == 0) {
		if (ntfs_log_replay(fs) == ST_OK) {
			fs->replayed = 1;
			return ST_OK;
		}
	}
	fs->ro = want_ro || !fs->rw_ok;
	if (!fs->ro && ntfs_check_system_clusters(fs) != ST_OK) {
		fs->gate |= NTFS_GATE_BITMAP;
		ntfs_gate_eval(fs);
		fs->ro = 1;
	}
	return ST_OK;
}

/* ---- fstype ops ---- */

static int ntfs_fst_probe(struct block_device *bdev)
{
	return NTFS_DRIVER_READY && ntfs_probe_boot(bdev);
}

static int ntfs_fst_mount(vfs_mount_t *m, struct block_device *bdev,
			  unsigned long flags, const char *data)
{
	ntfs_fs_t *fs;
	ntfs_opts_t o;
	int r, want_ro = (flags & VFS_MS_RDONLY) != 0;

	if (!m || !bdev)
		return ST_INVALID;
	if (!NTFS_DRIVER_READY)
		return ST_UNSUPPORTED;
	ntfs_opts_default(&o);
	if (ntfs_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	if (!ntfs_probe_boot(bdev))
		return ST_NOT_FOUND;
	fs = (ntfs_fs_t *)kcalloc(1, sizeof(*fs));
	if (!fs)
		return ST_NOMEM;
	fs->opt = o;
	fs->mnt = m;
	r = ntfs_fs_init(fs, bdev);
	if (r == ST_OK)
		r = ntfs_load(fs, want_ro);
	if (r == ST_OK && fs->replayed) {
		/* $LogFile was replayed: everything loaded so far describes
		 * the volume before the replay.  Start over. */
		ntfs_fs_release(fs);
		mm_memset(fs, 0, sizeof(*fs));
		fs->opt = o;
		fs->mnt = m;
		r = ntfs_fs_init(fs, bdev);
		if (r == ST_OK)
			r = ntfs_load(fs, want_ro);
	}
	if (r != ST_OK) {
		ntfs_fs_release(fs);
		kfree(fs);
		return r;
	}
	if (!want_ro && fs->ro) {
		/* Silently degraded, with one line. */
		kprintf("ntfs: %s: mounted read-only: %s\n",
			bdev->name ? bdev->name : "?", fs->ro_reason);
		m->flags |= VFS_MS_RDONLY;
	}
	m->ops = &ntfs_vfs_ops;
	m->fs_data = fs;
	m->sb = &fs->sb;
	return ST_OK;
}

/* Last step of an unmount (the VFS stored pages and inodes and called
 * sync already): commit what is left, then -- only after every commit
 * and flush succeeded and no error was ever recorded -- clear the dirty
 * flag (the dirty state is decided after the final commits).
 * A mount that never wrote writes nothing here either. */
static int ntfs_fst_unmount(vfs_mount_t *m)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);

	if (!fs)
		return ST_INVALID;
	if (!fs->ro) {
		int r = ntfs_settle(fs, 1);

		if (r != ST_OK)
			kprintf("ntfs: %s: unmounted with errors (%d); the volume "
				"stays marked dirty\n",
				fs->bdev->name ? fs->bdev->name : "?", r);
	}
	WARN_ON(fs->armed && !fs->latched_ro && !fs->errors && !fs->io_errs);
	ntfs_fs_release(fs);
	m->fs_data = NULL;
	m->sb = NULL;
	kfree(fs);
	return ST_OK;
}

static int ntfs_fst_remount(vfs_mount_t *m, unsigned long flags,
			    const char *data)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_opts_t o;
	int names_changed, r = ST_OK;

	if (!fs)
		return ST_INVALID;
	o = fs->opt;
	if (data && ntfs_parse_options(data, &o) != ST_OK)
		return ST_INVALID;
	/* Only a real change is refused. */
	if (o.streams != fs->opt.streams ||
	    o.mft_zone_mult != fs->opt.mft_zone_mult) {
		kprintf("ntfs: %s: streams_interface and mft_zone_multiplier "
			"cannot change on remount\n",
			fs->bdev->name);
		return ST_INVALID;
	}
	if (!(flags & VFS_MS_RDONLY) && fs->ro) {
		/* Read-only -> read-write: the gate again; `force' overrides
		 * only the dirty flag (never hibernation, errors, the log,
		 * the mirror or the geometry). */
		int old_force = fs->opt.force;

		fs->opt.force = o.force;
		ntfs_gate_eval(fs);
		if (!fs->rw_ok && (fs->gate & NTFS_GATE_LOG) && o.recover)
			/* The replay rebuilds every in-memory structure of the
			 * mount; a mounted (possibly busy) instance cannot do
			 * that. */
			kprintf("ntfs: %s: replaying the journal needs a fresh "
				"mount: unmount, then mount with -o recover\n",
				fs->bdev->name);
		if (!fs->rw_ok) {
			fs->opt.force = old_force;
			ntfs_gate_eval(fs);
			kprintf("ntfs: %s: stays read-only: %s\n", fs->bdev->name,
				fs->ro_reason ? fs->ro_reason : "read-only");
			return ST_ROFS;
		}
		if (ntfs_check_system_clusters(fs) != ST_OK) {
			fs->gate |= NTFS_GATE_BITMAP;
			ntfs_gate_eval(fs);
			return ST_ROFS;
		}
		ntfs_meta_wlock(fs);
		fs->ro = 0;
		ntfs_meta_wunlock(fs);
	} else if ((flags & VFS_MS_RDONLY) && !fs->ro) {
		/* Read-write -> read-only: everything stored and the volume
		 * marked clean, or the remount fails and the mount stays
		 * read-write. */
		r = ntfs_settle(fs, 1);
		if (r != ST_OK) {
			kprintf("ntfs: %s: remount read-only failed (%d)\n",
				fs->bdev->name, r);
			return r == ST_ROFS ? ST_IO : r;
		}
		ntfs_meta_wlock(fs);
		fs->ro = 1;
		ntfs_meta_wunlock(fs);
	}
	ntfs_meta_wlock(fs);
	names_changed = o.nocase != fs->opt.nocase;
	fs->opt = o;
	fs->sf_valid = 0;
	ntfs_gate_eval(fs);
	ntfs_meta_wunlock(fs);
	if (names_changed)
		dcache_invalidate_owner(&fs->sb);
	return ST_OK;
}

static long ntfs_fst_show_options(vfs_mount_t *m, char *buf, long cap)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	const ntfs_opts_t *o;
	long n = 0;
	int k;

	if (!fs || !buf || cap < 1)
		return 0;
	o = &fs->opt;
#define NTFS_SHOW(...)                                                       \
	do {                                                                 \
		if (n < cap - 1) {                                           \
			k = ksnprintf(buf + n, (size_t)(cap - n), __VA_ARGS__); \
			if (k > 0)                                           \
				n += k;                                      \
			if (n > cap - 1)                                     \
				n = cap - 1;                                 \
		}                                                            \
	} while (0)
	NTFS_SHOW("uid=%u,gid=%u,fmask=%04o,dmask=%04o", (unsigned)o->uid,
		  (unsigned)o->gid, (unsigned)o->fmask, (unsigned)o->dmask);
	if (o->force)
		NTFS_SHOW(",force");
	if (o->recover)
		NTFS_SHOW(",recover");
	if (o->discard)
		NTFS_SHOW(",discard");
	if (o->acl)
		NTFS_SHOW(",acl");
	if (o->nocase)
		NTFS_SHOW(",nocase");
	if (o->sparse)
		NTFS_SHOW(",sparse");
	if (o->show_sys)
		NTFS_SHOW(",show_sys_files");
	if (o->hide_dot)
		NTFS_SHOW(",hide_dot_files");
	if (o->nohidden)
		NTFS_SHOW(",nohidden");
	if (o->sys_immutable)
		NTFS_SHOW(",sys_immutable");
	if (o->windows_names)
		NTFS_SHOW(",windows_names");
	if (o->native_symlink == NTFS_SYMLINK_REL)
		NTFS_SHOW(",native_symlink=rel");
	if (o->symlink_native)
		NTFS_SHOW(",symlink=native");
	if (o->streams == NTFS_STREAMS_WINDOWS)
		NTFS_SHOW(",streams_interface=windows");
	else if (o->streams == NTFS_STREAMS_NONE)
		NTFS_SHOW(",streams_interface=none");
	if (o->errors == NTFS_ERRORS_CONTINUE)
		NTFS_SHOW(",errors=continue");
	else if (o->errors == NTFS_ERRORS_PANIC)
		NTFS_SHOW(",errors=panic");
	if (o->mft_zone_mult != 1)
		NTFS_SHOW(",mft_zone_multiplier=%d", o->mft_zone_mult);
	if (o->prealloc != 64 * 1024)
		NTFS_SHOW(",preallocated_size=%llu",
			  (unsigned long long)o->prealloc);
#undef NTFS_SHOW
	return n;
}

const vfs_fstype_t ntfs_fstype = {
	.name = "ntfs",
	.aliases = NULL,
	.needs_device = 1,
	.mount = ntfs_fst_mount,
	.unmount = ntfs_fst_unmount,
	.remount = ntfs_fst_remount,
	.show_options = ntfs_fst_show_options,
	.probe = ntfs_fst_probe,
};
