/* LikeOS - NTFS filesystem driver
 *
 * MFT records: fixups, validation, record cache, writes, allocation.
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
 * S5  Multi-sector transfer protection (update sequence arrays)
 *
 * A FILE, INDX or RSTR record of `size' bytes is split into 512-byte
 * strides; the last two bytes of every stride were replaced on disk by the
 * update sequence number (USN, the first word of the array) and saved in
 * the array.  ntfs_mst_fixup() checks the array geometry against the
 * record size (never trusting usa_count alone), checks
 * that every stride ends in the USN (a torn write otherwise) and puts the
 * saved words back.
 * =========================================================================== */

/* ST_OK, or ST_IO when the record is not intact.  `buf' must hold `size'
 * bytes, `size' a multiple of 512 and at least 512. */
int ntfs_mst_fixup(uint8_t *buf, uint32_t size)
{
	uint16_t usa_ofs = ntfs_le16(buf + 4);
	uint16_t usa_count = ntfs_le16(buf + 6);
	uint32_t strides = size / NTFS_BLOCK_SIZE;
	uint16_t usn;

	if (size < NTFS_BLOCK_SIZE || (size & (NTFS_BLOCK_SIZE - 1)))
		return ST_IO;
	/* The array is the USN plus one word per stride, starts on an even
	 * offset after the 8-byte magic/offset/count words and must end in
	 * the first stride (it is not itself protected). */
	if ((uint32_t)usa_count != strides + 1 || (usa_ofs & 1) ||
	    usa_ofs < 8 ||
	    (uint32_t)usa_ofs + (uint32_t)usa_count * 2U > NTFS_BLOCK_SIZE - 2)
		return ST_IO;
	usn = ntfs_le16(buf + usa_ofs);
	/* 0 and 0xFFFF are never written as a USN. */
	if (usn == 0 || usn == 0xFFFF)
		return ST_IO;
	for (uint32_t i = 0; i < strides; i++) {
		uint8_t *end = buf + (i + 1) * NTFS_BLOCK_SIZE - 2;

		if (ntfs_le16(end) != usn)
			return ST_IO;
	}
	for (uint32_t i = 0; i < strides; i++) {
		uint8_t *end = buf + (i + 1) * NTFS_BLOCK_SIZE - 2;
		const uint8_t *saved = buf + usa_ofs + 2 + i * 2;

		end[0] = saved[0];
		end[1] = saved[1];
	}
	return ST_OK;
}

/* The inverse, for writers: bump the USN, save the last word of every
 * stride and replace it.  ST_IO when the array geometry is invalid (then
 * the buffer is untouched). */
int ntfs_mst_protect(uint8_t *buf, uint32_t size)
{
	uint16_t usa_ofs = ntfs_le16(buf + 4);
	uint16_t usa_count = ntfs_le16(buf + 6);
	uint32_t strides = size / NTFS_BLOCK_SIZE;
	uint16_t usn;

	if (size < NTFS_BLOCK_SIZE || (size & (NTFS_BLOCK_SIZE - 1)) ||
	    (uint32_t)usa_count != strides + 1 || (usa_ofs & 1) ||
	    usa_ofs < 8 ||
	    (uint32_t)usa_ofs + (uint32_t)usa_count * 2U > NTFS_BLOCK_SIZE - 2)
		return ST_IO;
	usn = (uint16_t)(ntfs_le16(buf + usa_ofs) + 1);
	if (usn == 0 || usn == 0xFFFF)
		usn = 1;
	ntfs_put_le16(buf + usa_ofs, usn);
	for (uint32_t i = 0; i < strides; i++) {
		uint8_t *end = buf + (i + 1) * NTFS_BLOCK_SIZE - 2;
		uint8_t *saved = buf + usa_ofs + 2 + i * 2;

		saved[0] = end[0];
		saved[1] = end[1];
		ntfs_put_le16(end, usn);
	}
	return ST_OK;
}

/* ===========================================================================
 * S7, S8  MFT record and attribute validation
 *
 * The checks each validator makes are listed in the section banners of
 * S7 MFT records (below) and S8 Attributes (attrib.c); the readers, the
 * search context and the record writer there call these.
 * =========================================================================== */

/* Validate and fix up the raw record in `buf' (record number `no'). */
int ntfs_mft_rec_check(ntfs_fs_t *fs, uint8_t *buf, uint64_t no)
{
	ntfs_mft_record_t *m = (ntfs_mft_record_t *)buf;
	uint32_t usa_end, in_use, aoff;
	int any = 0;

	if (m->magic != NTFS_MAGIC_FILE) {
		for (uint32_t i = 0; i < fs->rec_size && !any; i++)
			any = buf[i] != 0;
		if (!any)
			return ST_NOT_FOUND; /* never initialised */
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: bad magic 0x%x%s",
			       (unsigned long long)no, m->magic,
			       m->magic == NTFS_MAGIC_BAAD ? " (BAAD)" : "");
		return ST_IO;
	}
	if (ntfs_mst_fixup(buf, fs->rec_size) != ST_OK) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: update sequence "
				  "mismatch (torn or corrupt record)",
			       (unsigned long long)no);
		return ST_IO;
	}
	usa_end = (uint32_t)m->usa_ofs + (uint32_t)m->usa_count * 2U;
	in_use = m->bytes_in_use;
	aoff = m->attrs_offset;
	if (m->usa_ofs < NTFS_MFT_REC_HDR_OLD - 2 ||
	    m->bytes_allocated != fs->rec_size || (aoff & 7) ||
	    aoff < usa_end || (in_use & 7) || in_use > fs->rec_size ||
	    (uint64_t)aoff + 4 > in_use) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt header "
				  "(attrs %u in use %u allocated %u)",
			       (unsigned long long)no, aoff, in_use,
			       m->bytes_allocated);
		return ST_IO;
	}
	if (NTFS_MREF_NO(m->base_mft_record) >= NTFS_MAX_MFT_NO) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: bad base reference",
			       (unsigned long long)no);
		return ST_IO;
	}
	/* NTFS 3.1 records carry their own number. */
	if (m->usa_ofs >= NTFS_MFT_REC_HDR && m->mft_record_number &&
	    m->mft_record_number != (uint32_t)no)
		WARN_RATELIMIT(1, "ntfs: MFT record %llu claims to be %u",
			       (unsigned long long)no, m->mft_record_number);
	return ST_OK;
}

/* Re-validate a record about to be written (fixed-up form): the header as
 * ntfs_mft_rec_check() demands it, every attribute through the reader's
 * validator, attributes in order, the end marker exactly where
 * bytes_in_use says.  Nothing that a later read would refuse is ever
 * written. */
int ntfs_rec_verify(ntfs_fs_t *fs, const uint8_t *rec, uint64_t no)
{
	const ntfs_mft_record_t *m = (const ntfs_mft_record_t *)rec;
	uint32_t usa_end = (uint32_t)m->usa_ofs + (uint32_t)m->usa_count * 2U;
	uint32_t off = m->attrs_offset, prev_type = 0;

	if (m->magic != NTFS_MAGIC_FILE ||
	    (uint32_t)m->usa_count != fs->rec_size / NTFS_BLOCK_SIZE + 1 ||
	    (m->usa_ofs & 1) || m->usa_ofs < NTFS_MFT_REC_HDR_OLD - 2 ||
	    usa_end > NTFS_BLOCK_SIZE - 2 ||
	    m->bytes_allocated != fs->rec_size || (off & 7) || off < usa_end ||
	    (m->bytes_in_use & 7) || m->bytes_in_use > fs->rec_size ||
	    (uint64_t)off + 8 > m->bytes_in_use)
		goto bad;
	if (!(m->flags & NTFS_MFT_REC_IN_USE))
		return ST_OK; /* a freed record: only its header matters */
	for (;;) {
		const ntfs_attr_record_t *a;

		if ((uint64_t)off + 4 > m->bytes_in_use)
			goto bad;
		a = (const ntfs_attr_record_t *)(rec + off);
		if (a->type == NTFS_AT_END) {
			if (off + 8 != m->bytes_in_use)
				goto bad;
			return ST_OK;
		}
		if (ntfs_attr_valid(fs, rec, off, no) != ST_OK)
			goto bad;
		if (a->type < prev_type)
			goto bad;
		prev_type = a->type;
		off += a->length;
	}
bad:
	WARN(1, "ntfs: refusing to write MFT record %llu: it would not pass "
		"validation",
	     (unsigned long long)no);
	return ST_IO;
}

/* ===========================================================================
 * S7  MFT records
 *
 * ntfs_mft_read(fs, no, buf) hands out a private copy of record `no' that
 * is fixed up and structurally valid:
 *
 *   magic "FILE" ("BAAD" or anything else: corrupt), update sequence array
 *   consistent with the record size, bytes_allocated == record size,
 *   attrs_offset 8-aligned and behind the array, bytes_in_use 8-aligned,
 *   within the record and covering at least the 4-byte type at
 *   attrs_offset.
 *
 * A record that was never initialised (all zeros, or beyond $MFT's
 * initialized size) is reported as ST_NOT_FOUND ("not in use").  Whether a
 * record is in use / a base record is the caller's question.
 *
 * Copies come from the record cache (NTFS_RC_ENTRIES fixed-up records,
 * LRU, hashed by record number, rc_lock); a miss reads the record through
 * the page cache of the $MFT stream (reserved handle NTFS_HANDLE_MFT) and
 * validates it before it is cached.  During the mount bootstrap, before
 * the $MFT stream exists, ntfs_mft_read_raw() reads straight from a given
 * runlist.
 * =========================================================================== */

static inline int ntfs_rc_bucket(uint64_t no)
{
	return (int)((no * 0x9E3779B97F4A7C15ULL) >> 53) & (NTFS_RC_HASH - 1);
}

/* Copy a cached record out; 1 on a hit. */
static int ntfs_rc_lookup(ntfs_fs_t *fs, uint64_t no, uint8_t *buf)
{
	uint64_t fl;
	int hit = 0;

	spin_lock_irqsave(&fs->rc_lock, &fl);
	for (int i = fs->rc_hash[ntfs_rc_bucket(no)]; i >= 0;
	     i = fs->rc[i].next) {
		if (fs->rc[i].no == no && fs->rc[i].data) {
			mm_memcpy(buf, fs->rc[i].data, fs->rec_size);
			fs->rc[i].stamp = ++fs->rc_clock;
			hit = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->rc_lock, fl);
	return hit;
}

/* Unlink entry `e' from its hash chain (rc_lock held). */
static void ntfs_rc_unhash(ntfs_fs_t *fs, int e)
{
	int b = ntfs_rc_bucket(fs->rc[e].no);
	int *pp = &fs->rc_hash[b];

	lockdep_assert_held(&fs->rc_lock);
	while (*pp >= 0) {
		if (*pp == e) {
			*pp = fs->rc[e].next;
			break;
		}
		pp = &fs->rc[*pp].next;
	}
	fs->rc[e].next = -1;
	fs->rc[e].no = ~0ULL;
}

/* Remember a validated copy of record `no'. */
static void ntfs_rc_insert(ntfs_fs_t *fs, uint64_t no, const uint8_t *buf)
{
	uint8_t *spare = NULL;
	uint64_t fl, best = ~0ULL;
	int victim = -1;

	/* A buffer for an empty slot is allocated outside the lock. */
	spare = (uint8_t *)kalloc(fs->rec_size);
	spin_lock_irqsave(&fs->rc_lock, &fl);
	for (int i = fs->rc_hash[ntfs_rc_bucket(no)]; i >= 0;
	     i = fs->rc[i].next) {
		if (fs->rc[i].no == no) {
			/* Raced with another reader: same content. */
			spin_unlock_irqrestore(&fs->rc_lock, fl);
			ntfs_free(spare);
			return;
		}
	}
	for (int i = 0; i < NTFS_RC_ENTRIES; i++) {
		if (fs->rc[i].no == ~0ULL) {
			if (fs->rc[i].data || spare) {
				victim = i;
				break;
			}
			continue;
		}
		if (fs->rc[i].stamp < best) {
			best = fs->rc[i].stamp;
			victim = i;
		}
	}
	if (victim < 0) {
		spin_unlock_irqrestore(&fs->rc_lock, fl);
		ntfs_free(spare);
		return;
	}
	if (fs->rc[victim].no != ~0ULL)
		ntfs_rc_unhash(fs, victim);
	if (!fs->rc[victim].data) {
		fs->rc[victim].data = spare;
		spare = NULL;
	}
	mm_memcpy(fs->rc[victim].data, buf, fs->rec_size);
	fs->rc[victim].no = no;
	fs->rc[victim].stamp = ++fs->rc_clock;
	fs->rc[victim].next = fs->rc_hash[ntfs_rc_bucket(no)];
	fs->rc_hash[ntfs_rc_bucket(no)] = victim;
	spin_unlock_irqrestore(&fs->rc_lock, fl);
	ntfs_free(spare);
}

/* Forget record `no' (after rewriting it, or a record found corrupt). */
static void ntfs_rc_drop(ntfs_fs_t *fs, uint64_t no)
{
	uint64_t fl;

	spin_lock_irqsave(&fs->rc_lock, &fl);
	for (int i = fs->rc_hash[ntfs_rc_bucket(no)]; i >= 0;
	     i = fs->rc[i].next) {
		if (fs->rc[i].no == no) {
			ntfs_rc_unhash(fs, i);
			break;
		}
	}
	spin_unlock_irqrestore(&fs->rc_lock, fl);
}

void ntfs_rc_init(ntfs_fs_t *fs)
{
	for (int i = 0; i < NTFS_RC_ENTRIES; i++) {
		fs->rc[i].no = ~0ULL;
		fs->rc[i].data = NULL;
		fs->rc[i].next = -1;
		fs->rc[i].stamp = 0;
	}
	for (int i = 0; i < NTFS_RC_HASH; i++)
		fs->rc_hash[i] = -1;
	spinlock_init(&fs->rc_lock, "ntfs_rc");
}

void ntfs_rc_release(ntfs_fs_t *fs)
{
	for (int i = 0; i < NTFS_RC_ENTRIES; i++) {
		ntfs_free(fs->rc[i].data);
		fs->rc[i].data = NULL;
		fs->rc[i].no = ~0ULL;
	}
}

/* Read record `no' straight from the device through runlist `rl' (the
 * bootstrap $MFT runlist), validated.  No caching. */
int ntfs_mft_read_raw(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
		      uint64_t no, uint8_t *buf)
{
	uint64_t off, done = 0;
	uint8_t *tmp = NULL;
	int r = ST_OK;

	might_sleep();
	if (!ntfs_mul_ok(no, fs->rec_size, &off))
		return ST_IO;
	/* A record may span clusters (512-byte clusters, 1 KB records);
	 * read it cluster by cluster. */
	tmp = (uint8_t *)kalloc(fs->cluster_size);
	if (!tmp)
		return ST_NOMEM;
	while (done < fs->rec_size) {
		uint64_t pos = off + done;
		int64_t vcn = (int64_t)(pos >> fs->cluster_bits);
		uint32_t in = (uint32_t)(pos & (fs->cluster_size - 1));
		uint32_t n = fs->cluster_size - in;
		int64_t lcn;

		if (n > fs->rec_size - done)
			n = (uint32_t)(fs->rec_size - done);
		r = ntfs_rl_map(rl, vcn, &lcn, NULL);
		if (r != ST_OK || lcn == NTFS_LCN_HOLE) {
			r = ST_IO; /* $MFT is never sparse */
			break;
		}
		r = ntfs_read_clusters(fs, lcn, 1, tmp);
		if (r != ST_OK)
			break;
		mm_memcpy(buf + done, tmp + in, n);
		done += n;
	}
	ntfs_free(tmp);
	if (r != ST_OK)
		return r;
	return ntfs_mft_rec_check(fs, buf, no);
}

/* A validated, fixed-up copy of record `no' in `buf' (rec_size bytes).
 * ST_NOT_FOUND: beyond the MFT or never initialised; ST_IO: corrupt or
 * unreadable. */
int ntfs_mft_read(ntfs_fs_t *fs, uint64_t no, uint8_t *buf)
{
	int r;

	might_sleep();
	if (no >= fs->mft_records || !fs->st_mft)
		return ST_NOT_FOUND;
	if (ntfs_rc_lookup(fs, no, buf))
		return ST_OK;
	if (no << fs->rec_bits >= fs->st_mft->init_size)
		return ST_NOT_FOUND; /* reads as zeros: never initialised */
	r = ntfs_stream_pread(fs, fs->st_mft, no << fs->rec_bits,
			      fs->rec_size, buf, 0);
	if (r != ST_OK)
		return r == ST_NOMEM ? ST_NOMEM : ST_IO;
	r = ntfs_mft_rec_check(fs, buf, no);
	if (r != ST_OK)
		return r;
	ntfs_rc_insert(fs, no, buf);
	return ST_OK;
}

/* ---- record cache / index cache stores ---- */

/* Replace (or add) the cached copy of record `no'. */
static void ntfs_rc_store(ntfs_fs_t *fs, uint64_t no, const uint8_t *buf)
{
	uint64_t fl;
	int done = 0;

	spin_lock_irqsave(&fs->rc_lock, &fl);
	for (int i = fs->rc_hash[ntfs_rc_bucket(no)]; i >= 0;
	     i = fs->rc[i].next) {
		if (fs->rc[i].no == no && fs->rc[i].data) {
			mm_memcpy(fs->rc[i].data, buf, fs->rec_size);
			fs->rc[i].stamp = ++fs->rc_clock;
			done = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->rc_lock, fl);
	if (!done)
		ntfs_rc_insert(fs, no, buf);
}

/* Replace (or add) the cached copy of index block `vcn' of stream
 * `handle'; forget it (ntfs_ic_forget) when it is freed. */
void ntfs_ic_store(ntfs_fs_t *fs, uint32_t handle, int64_t vcn,
		   const uint8_t *buf, uint32_t size)
{
	uint64_t fl;
	int done = 0;

	spin_lock_irqsave(&fs->ic_lock, &fl);
	for (int i = fs->ic_hash[ntfs_ic_bucket(handle, vcn)]; i >= 0;
	     i = fs->ic[i].next) {
		if (fs->ic[i].handle == handle && fs->ic[i].vcn == vcn &&
		    fs->ic[i].data) {
			if (fs->ic[i].size == size) {
				mm_memcpy(fs->ic[i].data, buf, size);
				fs->ic[i].stamp = ++fs->ic_clock;
				done = 1;
			} else {
				ntfs_ic_unhash(fs, i);
			}
			break;
		}
	}
	spin_unlock_irqrestore(&fs->ic_lock, fl);
	if (!done)
		ntfs_ic_insert(fs, handle, vcn, buf, size);
}

void ntfs_ic_forget(ntfs_fs_t *fs, uint32_t handle, int64_t vcn)
{
	uint64_t fl;

	spin_lock_irqsave(&fs->ic_lock, &fl);
	for (int i = fs->ic_hash[ntfs_ic_bucket(handle, vcn)]; i >= 0;
	     i = fs->ic[i].next) {
		if (fs->ic[i].handle == handle && fs->ic[i].vcn == vcn) {
			ntfs_ic_unhash(fs, i);
			break;
		}
	}
	spin_unlock_irqrestore(&fs->ic_lock, fl);
}

/* ---- MFT record writes ---- */

/* Write the mirror copy of record `no' (already protected) to $MFTMirr
 * (one helper for every mirror write, whole device blocks). */
static int ntfs_mirr_write(ntfs_fs_t *fs, uint64_t no, const uint8_t *prot)
{
	if (no >= fs->mirr_recs || !fs->mirr_rl.count)
		return ST_OK;
	return ntfs_rl_pwrite_rmw(fs, &fs->mirr_rl, no << fs->rec_bits, prot,
				  fs->rec_size);
}

/* Write MFT record `no' from `rec' (fixed-up form, rec_size bytes).  The
 * record gets a new update sequence number, which is also stored back into
 * `rec' so that the caller's copy matches the device.  Writes the whole
 * I/O unit, the mirror (records below fs->mirr_recs), and refreshes the
 * record cache.  A failed write latches the mount read-only. */
int ntfs_mft_write_ex(ntfs_fs_t *fs, uint64_t no, uint8_t *rec,
		      int mirror_first)
{
	uint8_t *prot;
	uint64_t off;
	int r;

	might_sleep();
	ntfs_assert_meta_excl(fs);
	if (no >= fs->mft_records || !fs->st_mft)
		return ST_IO;
	if (ntfs_rec_verify(fs, rec, no) != ST_OK)
		return ntfs_fs_error(fs, no, "record failed re-validation");
	prot = (uint8_t *)kalloc(fs->rec_size);
	if (!prot)
		return ST_NOMEM;
	mm_memcpy(prot, rec, fs->rec_size);
	if (ntfs_mst_protect(prot, fs->rec_size) != ST_OK) {
		kfree(prot);
		return ntfs_fs_error(fs, no, "bad update sequence array");
	}
	/* The new USN, into the caller's copy too. */
	{
		uint16_t usa = ((ntfs_mft_record_t *)rec)->usa_ofs;

		rec[usa] = prot[usa];
		rec[usa + 1] = prot[usa + 1];
	}
	off = no << fs->rec_bits;
	/* `mirror_first': the write that marks the volume clean.  The mirror
	 * copy goes first and the record only when that copy is on the
	 * device: a failed mirror write or a power cut between the two then
	 * leaves $MFT saying dirty (the next system checks the volume), never
	 * a volume marked clean next to a stale mirror. */
	if (mirror_first) {
		r = ntfs_mirr_write(fs, no, prot);
		if (r == ST_OK)
			r = ntfs_dev_flush(fs);
		if (r != ST_OK) {
			kfree(prot);
			ntfs_fs_error(fs, no, "$MFTMirr could not be updated");
			return ST_IO;
		}
	}
	/* The I/O unit around the record: ntfs_wt_write() reads the
	 * other records of a larger device block from the device and
	 * rewrites them unchanged. */
	r = ntfs_wt_write(fs, fs->st_mft, off, prot, fs->rec_size);
	if (r != ST_OK) {
		kfree(prot);
		ntfs_rc_drop(fs, no);
		return r;
	}
	/* Order: $MFT first, then $MFTMirr.  A power cut between the two
	 * leaves the mirror one update behind; the next mount sees the
	 * mismatch and stays read-only until a check (an availability cost
	 * accepted on purpose: telling which copy is newer is not possible
	 * from the records alone, and the mirror comparison is the guard
	 * against mounting a damaged $MFT read-write).  Writing the mirror
	 * first would not help: the cut would leave the same mismatch.
	 *
	 * The record is on the device: the write happened, whatever the
	 * mirror does.  A mirror copy that cannot be written (no memory for
	 * a partial-sector bounce, a device error) leaves $MFTMirr behind;
	 * that is recorded as an error (the volume stays dirty, the next
	 * mount sees the mismatch and stays read-only until a check), never
	 * reported as a failed record write. */
	if (!mirror_first && ntfs_mirr_write(fs, no, prot) != ST_OK)
		ntfs_fs_error(fs, no, "$MFTMirr could not be updated");
	kfree(prot);
	ntfs_rc_store(fs, no, rec);
	return ST_OK;
}

int ntfs_mft_write(ntfs_fs_t *fs, uint64_t no, uint8_t *rec)
{
	return ntfs_mft_write_ex(fs, no, rec, 0);
}

/* ===========================================================================
 * S25  MFT records: allocation, formatting, freeing, $MFT growth
 *
 * $MFT:$BITMAP has one bit per record.  Records 0..23 are the system's
 * (16..23 reserved): user records are taken from 24 on, next fit.
 * A free record is reused with the sequence number it got when it was
 * freed (freeing increments it, skipping 0), so a stale reference to the
 * old file is recognised; its old update sequence number is kept
 * only when the old record was valid.  Record numbers stay below
 * 2^32.
 *
 * Order: the bitmap bit is set before the record is written in use, and
 * cleared only after the record was written as not in use.
 *
 * $MFT growth: when no bit is clear, $MFT:$DATA grows by a few records'
 * worth of clusters (from the MFT zone, contiguous with its last run where
 * possible); the new records are formatted on the device (not in use)
 * before record 0 describes them, $MFT:$BITMAP grows to cover them, and
 * record 0 is rewritten in place -- its attributes stay where they are
 * (no new extension records for $MFT, whose own records must be readable
 * through what record 0 describes).  When record 0 has no room
 * for a longer runlist the growth fails with ENOSPC, nothing changed.
 * Records beyond $MFT's initialized size are formatted before the size is
 * raised over them.
 * =========================================================================== */

/* Format `buf' as an empty record `no' (not in use). */
static void ntfs_rec_format(ntfs_fs_t *fs, uint8_t *buf, uint64_t no,
			    uint16_t seq)
{
	ntfs_mft_record_t *m = (ntfs_mft_record_t *)buf;
	uint16_t usa = fs->new_usa_ofs;
	uint16_t cnt = (uint16_t)(fs->rec_size / NTFS_BLOCK_SIZE + 1);
	uint32_t ao = ((uint32_t)usa + 2U * cnt + 7U) & ~7U;

	mm_memset(buf, 0, fs->rec_size);
	m->magic = NTFS_MAGIC_FILE;
	m->usa_ofs = usa;
	m->usa_count = cnt;
	m->sequence_number = seq ? seq : 1;
	m->attrs_offset = (uint16_t)ao;
	m->bytes_in_use = ao + 8;
	m->bytes_allocated = fs->rec_size;
	if (usa >= NTFS_MFT_REC_HDR)
		m->mft_record_number = (uint32_t)no;
	ntfs_put_le16(buf + usa, 1); /* the USN; protect advances it */
	ntfs_put_le32(buf + ao, NTFS_AT_END);
}

/* Bits of $MFT:$BITMAP. */
static int ntfs_mftbm_get(ntfs_fs_t *fs, uint64_t bit, int *val)
{
	ntfs_stream_t *st = fs->st_mftbmp;
	pc_page_t *pg;
	uint64_t byte = bit / 8;
	int r;

	if (byte >= st->data_size)
		return ST_IO;
	r = ntfs_stream_page(fs, st, byte / PAGE_SIZE, &pg);
	if (r != ST_OK)
		return r;
	*val = (pg->data[byte % PAGE_SIZE] >> (bit & 7)) & 1;
	pagecache_put(pg);
	return ST_OK;
}


static int ntfs_mftbm_set(ntfs_fs_t *fs, uint64_t bit, int val)
{
	ntfs_stream_t *st = fs->st_mftbmp;
	uint64_t byte = bit / 8;
	pc_page_t *pg;
	uint8_t b;
	int r;

	if (byte >= st->data_size)
		return ST_IO;
	r = ntfs_stream_page(fs, st, byte / PAGE_SIZE, &pg);
	if (r != ST_OK)
		return r;
	b = pg->data[byte % PAGE_SIZE];
	pagecache_put(pg);
	if (val)
		b |= (uint8_t)(1U << (bit & 7));
	else
		b &= (uint8_t)~(1U << (bit & 7));
	if (st->flags & NTFS_ST_RESIDENT) {
		/* Record 0 is rewritten as a whole (in place also when $MFT
		 * has an attribute list: ntfs_eino_commit_mft()). */
		uint8_t old;

		if (byte >= st->res_len)
			return ST_IO;
		old = st->res_data[byte];
		st->res_data[byte] = b;
		r = ntfs_eino_commit_mft(fs, &fs->st_mft->rl,
					 fs->st_mft->data_size,
					 fs->st_mft->init_size);
		if (r != ST_OK)
			st->res_data[byte] = old;
		else
			pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
		return r;
	}
	return ntfs_wt_write(fs, st, byte, &b, 1);
}

/* Count the free records once. */
static int ntfs_mftbm_count(ntfs_fs_t *fs)
{
	uint64_t fr = 0;
	int r;

	if (fs->mftbm_valid)
		return ST_OK;
	r = ntfs_bitmap_free(fs, fs->st_mftbmp, fs->mft_records, &fr);
	if (r != ST_OK)
		return r;
	fs->free_records = fr;
	fs->mftbm_valid = 1;
	ntfs_sf_publish(fs);
	return ST_OK;
}

/* Write records [first, last] formatted and not in use, through the
 * runlist `rl' (records beyond what the stream covers yet).  Records of a
 * shared device block outside the range keep their contents. */
static int ntfs_mft_format_range(ntfs_fs_t *fs, const ntfs_runlist_t *rl,
				 uint64_t first, uint64_t last)
{
	uint8_t *buf;
	int r = ST_OK;

	buf = (uint8_t *)kalloc(fs->rec_size);
	if (!buf)
		return ST_NOMEM;
	for (uint64_t no = first; no <= last && r == ST_OK; no++) {
		ntfs_rec_format(fs, buf, no, 1);
		r = ntfs_mst_protect(buf, fs->rec_size);
		if (r == ST_OK)
			r = ntfs_rl_pwrite_rmw(fs, rl, no << fs->rec_bits, buf,
					       fs->rec_size);
	}
	kfree(buf);
	return r;
}

/* Grow $MFT (see the section comment). */
static int ntfs_mft_grow(ntfs_fs_t *fs)
{
	ntfs_stream_t *st = fs->st_mft, *bm = fs->st_mftbmp;
	ntfs_runlist_t nrl;
	ntfs_run_t *runs = NULL;
	uint32_t nruns = 0;
	uint64_t clusters, old_recs = fs->mft_records, new_recs, new_size;
	int64_t goal;
	int r;

	mm_memset(&nrl, 0, sizeof(nrl));
	clusters = ((uint64_t)NTFS_MFT_GROW_RECS << fs->rec_bits) >>
		   fs->cluster_bits;
	if (!clusters)
		clusters = 1;
	if (st->data_size != st->alloc_size || st->init_size != st->data_size) {
		/* Records allocated but not covered yet: use them first. */
		clusters = 0;
	}
	new_size = st->alloc_size + (clusters << fs->cluster_bits);
	new_recs = new_size >> fs->rec_bits;
	if (new_recs > NTFS_MAX_MFT_NO || new_recs <= old_recs)
		return ST_NOSPC;
	/* $MFT:$BITMAP must cover the new records (8-byte steps). */
	if (((new_recs + 63) / 64) * 8 > bm->data_size)
		return ST_NOSPC; /* grown below, before $MFT, when needed */
	goal = st->rl.count ? st->rl.runs[st->rl.count - 1].lcn +
				      st->rl.runs[st->rl.count - 1].len :
			      -1;
	/* Records larger than a cluster are written as one request only
	 * when their clusters are contiguous (a power cut can then not tear
	 * one record into an old and a new half): one run when possible. */
	r = ntfs_clusters_alloc(fs, goal, clusters,
				NTFS_AL_MFT | NTFS_AL_META |
					(fs->rec_size > fs->cluster_size ?
						 NTFS_AL_CONTIG :
						 0),
				&runs, &nruns);
	if (r == ST_NOSPC && fs->rec_size > fs->cluster_size)
		r = ntfs_clusters_alloc(fs, goal, clusters,
					NTFS_AL_MFT | NTFS_AL_META, &runs,
					&nruns);
	if (r != ST_OK)
		return r;
	for (uint32_t i = 0; i < st->rl.count && r == ST_OK; i++)
		r = ntfs_rl_append(&nrl, st->rl.runs[i].vcn,
				   st->rl.runs[i].lcn, st->rl.runs[i].len);
	for (uint32_t i = 0; i < nruns && r == ST_OK; i++)
		r = ntfs_rl_append(&nrl, nrl.end_vcn, runs[i].lcn,
				   runs[i].len);
	if (r == ST_OK)
		r = ntfs_mft_format_range(fs, &nrl, old_recs, new_recs - 1);
	if (r == ST_OK)
		r = ntfs_barrier(fs);
	if (r == ST_OK)
		r = ntfs_eino_commit_mft(fs, &nrl, new_size, new_size);
	if (r != ST_OK) {
		/* Nothing references the new clusters: give them back. */
		for (uint32_t i = 0; i < nruns; i++)
			ntfs_clusters_free(fs, runs[i].lcn,
					   (uint64_t)runs[i].len);
		ntfs_free(runs);
		ntfs_rl_free(&nrl);
		return r;
	}
	ntfs_free(runs);
	ntfs_rl_free(&st->rl);
	st->rl = nrl;
	st->alloc_size = st->data_size = st->init_size = new_size;
	fs->mft_records = new_recs;
	/* Pages of the old end showed zeros where records are now. */
	pagecache_invalidate_range(&fs->sb, ntfs_chain(st),
				   old_recs << fs->rec_bits);
	if (fs->mftbm_valid)
		fs->free_records += new_recs - old_recs;
	ntfs_sf_publish(fs);
	return ST_OK;
}

/* Allocate an MFT record for a new file (or an extension record).  On
 * success the bitmap bit is set and *no / *seq give the record and the
 * sequence number it is to be written with; the caller writes it (in use)
 * before anything refers to it, and gives it back with ntfs_mft_free() on
 * failure. */
int ntfs_mft_alloc(ntfs_fs_t *fs, uint64_t *no_out, uint16_t *seq_out)
{
	uint8_t *rec = NULL;
	uint64_t no = 0;
	int r, found = 0, val;

	ntfs_assert_meta_excl(fs);
	r = ntfs_mftbm_count(fs);
	if (r != ST_OK)
		return r;
	for (int attempt = 0; attempt < 3 && !found; attempt++) {
		uint64_t start = fs->mft_hint < NTFS_FIRST_FREE_REC ?
					 NTFS_FIRST_FREE_REC :
					 fs->mft_hint;

		for (int pass = 0; pass < 2 && !found; pass++) {
			uint64_t lo = pass ? NTFS_FIRST_FREE_REC : start;
			uint64_t hi = pass ? start : fs->mft_records;

			for (uint64_t b = lo; b < hi && b < NTFS_MAX_MFT_NO; b++) {
				r = ntfs_mftbm_get(fs, b, &val);
				if (r != ST_OK)
					return r;
				if (!val) {
					no = b;
					found = 1;
					break;
				}
				/* whole bytes in use: skip ahead */
				if (!(b & 7) && b + 8 <= hi) {
					pc_page_t *pg;

					r = ntfs_stream_page(fs, fs->st_mftbmp,
							     (b / 8) / PAGE_SIZE,
							     &pg);
					if (r != ST_OK)
						return r;
					if (pg->data[(b / 8) % PAGE_SIZE] == 0xFF)
						b += 7;
					pagecache_put(pg);
				}
			}
		}
		if (found)
			break;
		/* No free record: grow the bitmap if needed, then $MFT. */
		{
			uint64_t want = fs->mft_records + NTFS_MFT_GROW_RECS * 2;

			if (((want + 63) / 64) * 8 > fs->st_mftbmp->data_size) {
				r = ntfs_mftbm_grow(fs, ((want + 63) / 64) * 8);
				if (r != ST_OK)
					return r;
			}
		}
		r = ntfs_mft_grow(fs);
		if (r != ST_OK)
			return r;
	}
	if (!found)
		return ST_NOSPC;
	/* Records beyond the initialized size are formatted first. */
	if ((no << fs->rec_bits) >= fs->st_mft->init_size) {
		uint64_t first = fs->st_mft->init_size >> fs->rec_bits;
		uint64_t ninit = (no + 1) << fs->rec_bits;

		ninit = ntfs_min64(fs->st_mft->data_size,
				   (ninit + fs->cluster_size - 1) &
					   ~((uint64_t)fs->cluster_size - 1));
		r = ntfs_mft_format_range(fs, &fs->st_mft->rl, first,
					  (ninit >> fs->rec_bits) - 1);
		if (r == ST_OK)
			r = ntfs_barrier(fs);
		if (r == ST_OK)
			r = ntfs_eino_commit_mft(fs, &fs->st_mft->rl,
						 fs->st_mft->data_size, ninit);
		if (r != ST_OK)
			return r;
		pagecache_invalidate_range(&fs->sb, ntfs_chain(fs->st_mft),
					   fs->st_mft->init_size &
						   ~((uint64_t)PAGE_SIZE - 1));
		fs->st_mft->init_size = ninit;
	}
	r = ntfs_mftbm_set(fs, no, 1);
	if (r != ST_OK)
		return r;
	rec = (uint8_t *)kalloc(fs->rec_size);
	if (!rec) {
		ntfs_leaked(fs); /* the bit stays set: a leaked record */
		return ST_NOMEM;
	}
	*seq_out = 1;
	r = ntfs_mft_read(fs, no, rec);
	if (r == ST_OK) {
		const ntfs_mft_record_t *m = (const ntfs_mft_record_t *)rec;

		if (m->flags & NTFS_MFT_REC_IN_USE) {
			kfree(rec);
			return ntfs_fs_error(fs, no,
					     "record marked free is in use");
		}
		*seq_out = m->sequence_number ? m->sequence_number : 1;
	}
	kfree(rec);
	if (fs->free_records)
		fs->free_records--;
	fs->mft_hint = no + 1;
	ntfs_sf_publish(fs);
	*no_out = no;
	return ST_OK;
}

/* Mark record `no' not in use (its sequence number advanced) and give its
 * bit back -- in that order, with a barrier between. */
static int ntfs_mft_free_do(ntfs_fs_t *fs, uint64_t no)
{
	uint8_t *rec;
	ntfs_mft_record_t *m;
	int r;

	ntfs_assert_meta_excl(fs);
	if (no < NTFS_FILE_FIRST_USER || no >= fs->mft_records) {
		WARN(1, "ntfs: refusing to free MFT record %llu",
		     (unsigned long long)no);
		return ST_INVALID; /* never a system record */
	}
	rec = (uint8_t *)kalloc(fs->rec_size);
	if (!rec)
		return ST_NOMEM;
	r = ntfs_mft_read(fs, no, rec);
	if (r == ST_OK) {
		m = (ntfs_mft_record_t *)rec;
		m->flags &= (uint16_t)~(NTFS_MFT_REC_IN_USE |
					NTFS_MFT_REC_IS_DIRECTORY);
		m->sequence_number = (uint16_t)(m->sequence_number + 1);
		if (!m->sequence_number)
			m->sequence_number = 1;
		m->link_count = 0;
		r = ntfs_mft_write(fs, no, rec);
	} else if (r == ST_NOT_FOUND) {
		r = ST_OK; /* never initialised: only the bit */
	}
	kfree(rec);
	if (r == ST_OK)
		r = ntfs_barrier(fs);
	if (r == ST_OK)
		r = ntfs_mftbm_set(fs, no, 0);
	if (r == ST_OK) {
		fs->free_records++;
		if (no < fs->mft_hint)
			fs->mft_hint = no;
		ntfs_sf_publish(fs);
	}
	return r;
}

/* A record that cannot be given back stays allocated: a leak. */
int ntfs_mft_free(ntfs_fs_t *fs, uint64_t no)
{
	int r = ntfs_mft_free_do(fs, no);

	if (r != ST_OK)
		ntfs_leaked(fs);
	return r;
}
