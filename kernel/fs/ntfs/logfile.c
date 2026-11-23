/* LikeOS - NTFS filesystem driver
 *
 * $LogFile: reset, clean log, replay.
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

/* ---- $LogFile ----
 *
 * Writing without journaling means the log must not claim anything when
 * the other system mounts the volume next: the first write of a mount
 * resets $LogFile to the empty state (every byte 0xFF up to its initialized
 * size), which that system re-initialises; a log that is already empty is
 * left alone.  The fill is bounded by the runlist and the volume, never by
 * the initialized size alone, and written in
 * whole device sectors of exactly the log's own clusters (nothing of
 * the neighbouring $MFTMirr sectors of a 512-byte-cluster volume is
 * touched).  An unclean log never gets here: the gate keeps such a volume
 * read-only (only $LogFile replay, S33, may clean it). */
int ntfs_logfile_reset(ntfs_fs_t *fs)
{
	ntfs_stream_t *st = fs->st_logfile;
	uint64_t len, chunk = 64 * 1024, done = 0;
	uint8_t *buf;
	int r = ST_OK;

	if (!st || fs->log_state == NTFS_LOG_EMPTY)
		return ST_OK;
	if (st->flags & NTFS_ST_RESIDENT || fs->log_state == NTFS_LOG_UNCLEAN ||
	    fs->log_state == NTFS_LOG_BAD)
		return ST_IO;
	len = ntfs_min64(st->init_size, st->alloc_size);
	len = ntfs_min64(len, (uint64_t)st->rl.end_vcn << fs->cluster_bits);
	len &= ~((uint64_t)fs->dev_ss - 1);
	buf = (uint8_t *)kalloc(chunk);
	if (!buf)
		return ST_NOMEM;
	mm_memset(buf, 0xFF, chunk);
	while (done < len) {
		uint64_t n = ntfs_min64(chunk, len - done);

		r = ntfs_rl_pwrite_raw(fs, &st->rl, done, buf, n);
		if (r != ST_OK)
			break;
		done += n;
	}
	kfree(buf);
	if (r == ST_OK) {
		pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
		fs->log_state = NTFS_LOG_EMPTY;
	}
	return r;
}

/* ---- a clean $LogFile at clean unmount / remount read-only ----
 *
 * While the mount is armed the log holds 0xFF bytes (ntfs_logfile_reset()).
 * A system that finds a log like that must initialise it before it mounts
 * the volume, which fails on write-protected media.  So the clean-unmount
 * sequence (ntfs_settle()) writes what a clean shutdown leaves behind,
 * before the dirty flag is cleared:
 *
 *   - one log record page (the first page of the log area) holding a
 *     single client-restart record of client "NTFS" -- an empty checkpoint:
 *     no open attributes, no dirty pages, no transactions -- and the same
 *     page in both tail copies (log version 1.1 keeps the current tail page
 *     in two ping-pong pages, recognised by the last LSN they hold);
 *   - two identical restart pages (version 1.1) whose restart area says
 *     "volume is clean", with that one client in use and its restart LSN
 *     pointing at the checkpoint.
 *
 * The record pages are written and flushed before the restart pages that
 * refer to them.  Nothing is written for a volume with errors or a
 * hibernated one (the callers check).  The LSN continues the sequence of
 * the log found at mount (the next sequence number), or starts at
 * sequence 2 when the log was empty, as a fresh log does.  The page cache
 * of $LogFile is dropped afterwards (the writes bypass it). */
#define NTFS_LOG_PAGE 4096U
#define NTFS_LOG_USA_OFS_RSTR 0x1EU
#define NTFS_LOG_USA_OFS_RCRD 0x28U
#define NTFS_LOG_DATA_OFS 0x40U /* records start here in a record page */
#define NTFS_LOG_REC_HDR 0x30U /* log record header */
#define NTFS_LOG_CKPT_LEN 0x40U /* the empty NTFS restart (checkpoint) */
#define NTFS_LOG_RT_CHECKPOINT 2U
#define NTFS_LOG_PAGE_RECORD_END 0x1U

/* Fill the update sequence array header fields of a log page. */
static void ntfs_log_page_init(uint8_t *p, uint32_t magic, uint16_t usa_ofs)
{
	mm_memset(p, 0, NTFS_LOG_PAGE);
	ntfs_put_le32(p, magic);
	ntfs_put_le16(p + 4, usa_ofs);
	ntfs_put_le16(p + 6, NTFS_LOG_PAGE / NTFS_BLOCK_SIZE + 1);
	ntfs_put_le16(p + usa_ofs, 1); /* the update sequence number */
}

/* Write log page `p' (4096 bytes, unprotected) at byte `off' of the log,
 * protected with its update sequence array. */
static int ntfs_log_page_write(ntfs_fs_t *fs, uint64_t off, const uint8_t *p,
			       uint8_t *tmp)
{
	mm_memcpy(tmp, p, NTFS_LOG_PAGE);
	if (ntfs_mst_protect(tmp, NTFS_LOG_PAGE) != ST_OK)
		return ST_IO;
	return ntfs_rl_pwrite_raw(fs, &fs->st_logfile->rl, off, tmp,
				  NTFS_LOG_PAGE);
}

int ntfs_logfile_write_clean(ntfs_fs_t *fs)
{
	ntfs_stream_t *st = fs->st_logfile;
	uint64_t size, seq, lsn, first = 2 * NTFS_LOG_PAGE + 2 * NTFS_LOG_PAGE;
	uint32_t sbits, fbits;
	uint8_t *page = NULL, *tmp = NULL, *rec, *ra, *cl;
	int r;

	ntfs_assert_meta_excl(fs);
	if (!st || (st->flags & NTFS_ST_RESIDENT) || fs->hibernated)
		return ST_OK;
	size = ntfs_min64(st->data_size, st->init_size);
	size = ntfs_min64(size, (uint64_t)st->rl.end_vcn << fs->cluster_bits);
	size &= ~(uint64_t)(NTFS_LOG_PAGE - 1);
	if (size < first + 2 * NTFS_LOG_PAGE || (fs->dev_ss > NTFS_LOG_PAGE))
		return ST_OK; /* not a log this layout fits: left empty */
	sbits = ntfs_log_seq_bits(size);
	fbits = 64 - sbits;
	seq = 2;
	if (fs->log_current_lsn > 0)
		seq = ((uint64_t)fs->log_current_lsn >> fbits) + 1;
	if (seq < 2 || seq >= (1ULL << sbits) - 1)
		seq = 2;
	lsn = (seq << fbits) | ((first + NTFS_LOG_DATA_OFS) >> 3);
	page = (uint8_t *)kalloc(NTFS_LOG_PAGE);
	tmp = (uint8_t *)kalloc(NTFS_LOG_PAGE);
	if (!page || !tmp) {
		r = ST_NOMEM;
		goto out;
	}
	/* The record page: header, then one client-restart record. */
	ntfs_log_page_init(page, NTFS_MAGIC_RCRD, NTFS_LOG_USA_OFS_RCRD);
	ntfs_put_le64(page + 0x08, lsn); /* last LSN of the page */
	ntfs_put_le32(page + 0x10, NTFS_LOG_PAGE_RECORD_END);
	ntfs_put_le16(page + 0x14, 1); /* page count */
	ntfs_put_le16(page + 0x16, 1); /* page position */
	ntfs_put_le16(page + 0x18, NTFS_LOG_DATA_OFS + NTFS_LOG_REC_HDR +
					   NTFS_LOG_CKPT_LEN);
	ntfs_put_le64(page + 0x20, lsn); /* last record ending here */
	rec = page + NTFS_LOG_DATA_OFS;
	ntfs_put_le64(rec + 0x00, lsn); /* this LSN */
	ntfs_put_le32(rec + 0x18, NTFS_LOG_CKPT_LEN); /* client data */
	ntfs_put_le32(rec + 0x20, NTFS_LOG_RT_CHECKPOINT);
	/* client data: version 0.0, analysis starts at this checkpoint, no
	 * tables */
	ntfs_put_le64(rec + NTFS_LOG_REC_HDR + 0x08, lsn);
	/* The two tail copies first (their last LSN says which page they
	 * copy), then the page itself. */
	r = ntfs_log_page_write(fs, 2 * NTFS_LOG_PAGE, page, tmp);
	if (r == ST_OK)
		r = ntfs_log_page_write(fs, 3 * NTFS_LOG_PAGE, page, tmp);
	if (r == ST_OK)
		r = ntfs_log_page_write(fs, first, page, tmp);
	if (r == ST_OK)
		r = ntfs_dev_flush(fs);
	if (r != ST_OK)
		goto out;
	/* The restart pages. */
	ntfs_log_page_init(page, NTFS_MAGIC_RSTR, NTFS_LOG_USA_OFS_RSTR);
	ntfs_put_le32(page + 0x10, NTFS_LOG_PAGE); /* system page size */
	ntfs_put_le32(page + 0x14, NTFS_LOG_PAGE); /* log page size */
	ntfs_put_le16(page + 0x18, 0x30); /* restart area offset */
	ntfs_put_le16(page + 0x1A, 1); /* minor version */
	ntfs_put_le16(page + 0x1C, 1); /* major version */
	ra = page + 0x30;
	ntfs_put_le64(ra + 0x00, lsn); /* current LSN */
	ntfs_put_le16(ra + 0x08, 1); /* clients */
	ntfs_put_le16(ra + 0x0A, NTFS_LOGFILE_NO_CLIENT); /* free list */
	ntfs_put_le16(ra + 0x0C, 0); /* in use: client 0 */
	ntfs_put_le16(ra + 0x0E, NTFS_RESTART_VOLUME_IS_CLEAN);
	ntfs_put_le32(ra + 0x10, sbits);
	ntfs_put_le16(ra + 0x14, NTFS_RESTART_AREA_HDR +
					 NTFS_LOG_CLIENT_RECORD);
	ntfs_put_le16(ra + 0x16, NTFS_RESTART_AREA_HDR); /* client array */
	ntfs_put_le64(ra + 0x18, size); /* file size */
	ntfs_put_le32(ra + 0x20, NTFS_LOG_CKPT_LEN); /* last data length */
	ntfs_put_le16(ra + 0x24, NTFS_LOG_REC_HDR);
	ntfs_put_le16(ra + 0x26, NTFS_LOG_DATA_OFS);
	ntfs_put_le32(ra + 0x28, (uint32_t)seq); /* restart open count */
	cl = ra + NTFS_RESTART_AREA_HDR;
	ntfs_put_le64(cl + 0x00, lsn); /* oldest LSN */
	ntfs_put_le64(cl + 0x08, lsn); /* client restart LSN */
	ntfs_put_le16(cl + 0x10, NTFS_LOGFILE_NO_CLIENT); /* previous */
	ntfs_put_le16(cl + 0x12, NTFS_LOGFILE_NO_CLIENT); /* next */
	ntfs_put_le32(cl + 0x1C, 8); /* name length in bytes */
	ntfs_put_le16(cl + 0x20, 'N');
	ntfs_put_le16(cl + 0x22, 'T');
	ntfs_put_le16(cl + 0x24, 'F');
	ntfs_put_le16(cl + 0x26, 'S');
	r = ntfs_log_page_write(fs, 0, page, tmp);
	if (r == ST_OK)
		r = ntfs_log_page_write(fs, NTFS_LOG_PAGE, page, tmp);
	if (r == ST_OK)
		r = ntfs_dev_flush(fs);
	if (r == ST_OK) {
		fs->log_state = NTFS_LOG_CLEAN;
		fs->log_current_lsn = (int64_t)lsn;
	}
out:
	pagecache_invalidate_file(&fs->sb, ntfs_chain(st));
	ntfs_free(page);
	ntfs_free(tmp);
	return r;
}

/* ===========================================================================
 * S33  $LogFile replay
 *
 * A volume whose $LogFile says "not clean" was not shut down cleanly: the
 * log holds changes of metadata (MFT records, index blocks, bitmaps) that
 * may or may not have reached their pages, and transactions that may not
 * have finished.  Before such a volume is mounted read-write the log is
 * replayed, all or nothing:
 *
 *   1. the restart area (the newer of the two restart pages) names the
 *      client "NTFS" and its restart LSN: a client-restart record (the
 *      checkpoint) that lists where the analysis starts and the LSNs of
 *      the dumps of the open-attribute table, the attribute names, the
 *      dirty-page table and the transaction table;
 *   2. analysis: from the checkpoint to the end of the log every record is
 *      read (log pages validated through their update sequence arrays,
 *      the current tail taken from the ping-pong tail copies when they are
 *      newer, records spanning pages reassembled, the end of the log found
 *      where the next LSN is not the one that was written there); the
 *      transaction table follows commits, forgets, compensations and the
 *      ends of nested actions, the attribute table follows the attributes
 *      opened;
 *   3. redo: from the oldest LSN the client still needs, every update is
 *      applied in log order to an in-memory OVERLAY of the pages it names
 *      (MFT records and index blocks only when their page LSN is older
 *      than the record -- the update is not on them yet; bitmap and value
 *      updates of other attributes, which carry no LSN, are idempotent in
 *      log order);
 *   4. undo: every transaction that did not commit is rolled back through
 *      its undo chain, newest first;
 *   5. every changed page is checked as the reader would check it (MFT
 *      records by the record validator, index blocks structurally); then
 *      written -- MFT records with their mirror, index blocks with fresh
 *      update sequence numbers, other clusters as they are -- the device
 *      flushed, and only then a clean $LogFile written (S23).  A power cut
 *      in between leaves the log unclean: the next mount replays again,
 *      with the same result.
 *
 * Anything unexpected aborts the replay with NOTHING written and the mount
 * read-only: an operation this driver does not implement, a record or
 * page that fails validation, a target that is not where the volume's own
 * metadata says it is (every cluster a record names is checked against
 * the attribute's runlist), an overlay larger than NTFS_LR_MAX_BYTES, more
 * than NTFS_LR_MAX_RECORDS records, a log that ends before the restart
 * area says it does.  The replay never runs on hibernated volumes, on a
 * mount asked to be read-only, or when any reason other than the log keeps
 * the volume read-only.
 *
 * Layout facts used (log version 1.1 and 2.0):
 *   LSN = (sequence << file_bits) | (byte offset in the log >> 3), with
 *   file_bits = 64 - seq_number_bits of the restart area;
 *   the log records live in [first, file_size): first = 2 system pages +
 *   2 log pages (the tail copies) for 1.1, 34 log pages for 2.0;
 *   a log record page ("RCRD") has its records from log_page_data_offset;
 *   a record header is 0x30 bytes (this LSN, previous LSN of the client,
 *   undo-next LSN, client data length, client id, record type, transaction
 *   id, flags); the client data of an update starts with the operation
 *   header: redo / undo operation, redo / undo offset and length (offsets
 *   from the start of the client data), the target attribute (its offset
 *   in the open-attribute table), the number of clusters that follow, the
 *   record offset (where the structure starts: an attribute in an MFT
 *   record, an index block in its buffer), the attribute offset (inside
 *   that structure), the 512-byte index of the target inside its first
 *   cluster, the target VCN and the clusters' LCNs.
 * =========================================================================== */

#define NTFS_LR_MAX_BYTES (64UL * 1024 * 1024) /* overlay memory */
#define NTFS_LR_MAX_RECORDS (1U << 22) /* records walked in one pass */
#define NTFS_LR_MAX_OAT 2048U /* open attributes */
#define NTFS_LR_MAX_TX 4096U /* transactions */
#define NTFS_LR_MAX_REC (256U * 1024U) /* one log record */
#define NTFS_LR_HASH 1024U
#define NTFS_LR_RESTART_ALLOCATED 0xFFFFFFFFU /* restart table entry in use */

/* Operations (the client's redo / undo codes). */
enum {
	NTFS_LOP_NOOP = 0x00,
	NTFS_LOP_CLR = 0x01, /* compensation */
	NTFS_LOP_INIT_FILE_RECORD = 0x02,
	NTFS_LOP_DEALLOC_FILE_RECORD = 0x03,
	NTFS_LOP_WRITE_END_OF_RECORD = 0x04,
	NTFS_LOP_CREATE_ATTRIBUTE = 0x05,
	NTFS_LOP_DELETE_ATTRIBUTE = 0x06,
	NTFS_LOP_UPDATE_RESIDENT = 0x07,
	NTFS_LOP_UPDATE_NONRESIDENT = 0x08,
	NTFS_LOP_UPDATE_MAPPING_PAIRS = 0x09,
	NTFS_LOP_DELETE_DIRTY_CLUSTERS = 0x0A,
	NTFS_LOP_SET_SIZES = 0x0B,
	NTFS_LOP_ADD_INDEX_ROOT = 0x0C,
	NTFS_LOP_DELETE_INDEX_ROOT = 0x0D,
	NTFS_LOP_ADD_INDEX_ALLOC = 0x0E,
	NTFS_LOP_DELETE_INDEX_ALLOC = 0x0F,
	NTFS_LOP_WRITE_END_OF_INDEX = 0x10,
	NTFS_LOP_SET_VCN_ROOT = 0x11,
	NTFS_LOP_SET_VCN_ALLOC = 0x12,
	NTFS_LOP_UPDATE_NAME_ROOT = 0x13,
	NTFS_LOP_UPDATE_NAME_ALLOC = 0x14,
	NTFS_LOP_SET_BITS = 0x15,
	NTFS_LOP_CLEAR_BITS = 0x16,
	NTFS_LOP_HOT_FIX = 0x17,
	NTFS_LOP_END_TOP_LEVEL = 0x18,
	NTFS_LOP_PREPARE = 0x19,
	NTFS_LOP_COMMIT = 0x1A,
	NTFS_LOP_FORGET = 0x1B,
	NTFS_LOP_OPEN_ATTRIBUTE = 0x1C,
	NTFS_LOP_OAT_DUMP = 0x1D,
	NTFS_LOP_NAMES_DUMP = 0x1E,
	NTFS_LOP_DPT_DUMP = 0x1F,
	NTFS_LOP_TT_DUMP = 0x20,
	NTFS_LOP_UPDATE_DATA_ROOT = 0x21,
	NTFS_LOP_UPDATE_DATA_ALLOC = 0x22,
};

#define NTFS_LR_TX_ACTIVE 1
#define NTFS_LR_TX_PREPARED 2
#define NTFS_LR_TX_COMMITTED 3

/* An overlay page: a copy of an MFT record, an index block or a cluster,
 * in the form the operations work on (records and index blocks fixed up),
 * keyed by its byte offset on the volume. */
#define NTFS_LR_RAW 0
#define NTFS_LR_MFT 1
#define NTFS_LR_INDX 2

typedef struct ntfs_lr_page {
	uint64_t off; /* volume byte offset */
	uint32_t size;
	uint8_t kind; /* NTFS_LR_* */
	uint8_t valid; /* fixups applied (records / index blocks) */
	uint8_t dirty;
	uint64_t mft_no; /* NTFS_LR_MFT */
	uint8_t *data;
	struct ntfs_lr_page *next; /* hash chain */
	struct ntfs_lr_page *all; /* list of every page */
} ntfs_lr_page_t;

/* An open attribute (the client's table, normalised). */
typedef struct ntfs_lr_oat {
	uint32_t key; /* offset in the client's table */
	uint64_t mref;
	uint32_t type;
	uint32_t bytes_per_index;
	uint8_t name_len; /* UTF-16 units; 0xFF: name unknown */
	uint16_t name[NTFS_MAX_NAME_LEN];
} ntfs_lr_oat_t;

typedef struct ntfs_lr_tx {
	uint32_t key; /* transaction id (offset in the client's table) */
	uint8_t state;
	int64_t undo_next;
} ntfs_lr_tx_t;

/* A log record, reassembled, with its operation header decoded. */
typedef struct ntfs_lr_rec {
	int64_t lsn;
	int64_t prev_lsn;
	int64_t undo_next_lsn;
	uint32_t cdl; /* client data length */
	uint32_t type; /* record type */
	uint32_t tx;
	uint8_t *cd; /* client data (cdl bytes) */
	/* the operation header (when cdl >= 0x20) */
	uint16_t redo_op, undo_op;
	uint16_t redo_off, redo_len, undo_off, undo_len;
	uint16_t target_attr, lcns;
	uint16_t rec_off, attr_off, cluster_index;
	int64_t vcn;
	const uint8_t *lcn_list; /* lcns * 8 bytes */
} ntfs_lr_rec_t;

typedef struct ntfs_lr {
	ntfs_fs_t *fs;
	ntfs_stream_t *log;
	/* restart area */
	uint32_t sys_ps, ps, data_ofs, rec_hdr;
	int major;
	uint32_t seq_bits, file_bits;
	uint64_t file_size, first;
	int64_t current_lsn, restart_lsn, oldest_lsn, last_lsn;
	/* the log pages, read once */
	uint8_t *pagebuf; /* one page, fixed up */
	uint64_t pagebuf_off; /* ~0: none */
	uint8_t *tail[2]; /* the two tail copies (1.1), fixed up or NULL */
	/* tables */
	ntfs_lr_oat_t *oat;
	uint32_t noat;
	ntfs_lr_tx_t *tx;
	uint32_t ntx;
	/* overlay */
	ntfs_lr_page_t *hash[NTFS_LR_HASH];
	ntfs_lr_page_t *pages;
	uint64_t bytes;
	uint32_t records;
	const char *why; /* the reason of an abort */
} ntfs_lr_t;

static int ntfs_lr_fail(ntfs_lr_t *lr, const char *why)
{
	if (!lr->why)
		lr->why = why;
	return ST_IO;
}

/* ---- LSNs and log pages ---- */

static uint64_t ntfs_lr_lsn_off(const ntfs_lr_t *lr, int64_t lsn)
{
	return ((uint64_t)lsn & ((1ULL << lr->file_bits) - 1)) << 3;
}

static uint64_t ntfs_lr_lsn_seq(const ntfs_lr_t *lr, int64_t lsn)
{
	return (uint64_t)lsn >> lr->file_bits;
}

static int64_t ntfs_lr_make_lsn(const ntfs_lr_t *lr, uint64_t seq,
				uint64_t off)
{
	return (int64_t)((seq << lr->file_bits) | (off >> 3));
}

/* Read log page `off' (raw) into `buf' and undo its fixups; ST_IO when it
 * is not a valid record page. */
static int ntfs_lr_read_page(ntfs_lr_t *lr, uint64_t off, uint8_t *buf)
{
	int r;

	if (off + lr->ps > lr->file_size)
		return ST_IO;
	r = ntfs_rl_pread(lr->fs, &lr->log->rl, off, lr->ps,
			  lr->log->init_size, buf);
	if (r != ST_OK)
		return r;
	if (ntfs_le32(buf) != NTFS_MAGIC_RCRD ||
	    ntfs_le16(buf + 6) != lr->ps / NTFS_BLOCK_SIZE + 1)
		return ST_IO;
	return ntfs_mst_fixup(buf, lr->ps);
}

/* Log page `off', fixed up, in lr->pagebuf; a tail copy that names this
 * page and is newer wins (version 1.1 writes the current tail there
 * first). */
static int ntfs_lr_page(ntfs_lr_t *lr, uint64_t off, const uint8_t **out)
{
	int r;

	if (lr->pagebuf_off == off) {
		*out = lr->pagebuf;
		return ST_OK;
	}
	lr->pagebuf_off = ~0ULL;
	r = ntfs_lr_read_page(lr, off, lr->pagebuf);
	for (int i = 0; i < 2; i++) {
		const uint8_t *t = lr->tail[i];
		int64_t tl;

		if (!t)
			continue;
		/* A copy belongs to the page that holds its last record. */
		tl = (int64_t)ntfs_le64(t + 0x20);
		if (tl <= 0 ||
		    (ntfs_lr_lsn_off(lr, tl) & ~(uint64_t)(lr->ps - 1)) != off)
			continue;
		if (r != ST_OK ||
		    tl > (int64_t)ntfs_le64(lr->pagebuf + 0x20)) {
			mm_memcpy(lr->pagebuf, t, lr->ps);
			r = ST_OK;
		}
	}
	if (r != ST_OK)
		return r;
	lr->pagebuf_off = off;
	*out = lr->pagebuf;
	return ST_OK;
}

/* The log page after `off' (wrapping to the first one). *wrapped set when
 * it wrapped. */
static uint64_t ntfs_lr_next_page(const ntfs_lr_t *lr, uint64_t off,
				  int *wrapped)
{
	off += lr->ps;
	if (off >= lr->file_size) {
		off = lr->first;
		*wrapped = 1;
	}
	return off;
}

/* Read the record at `lsn' into `rec' (rec->cd: kalloc'd client data).
 * ST_NOT_FOUND when the log holds no record with that LSN there (the end
 * of the log), ST_IO when it is damaged.  *next: the LSN after it. */
static int ntfs_lr_read(ntfs_lr_t *lr, int64_t lsn, ntfs_lr_rec_t *rec,
			int64_t *next)
{
	uint64_t pos = ntfs_lr_lsn_off(lr, lsn), seq = ntfs_lr_lsn_seq(lr, lsn);
	uint64_t page = pos & ~(uint64_t)(lr->ps - 1), in = pos - page;
	uint32_t got = 0, total;
	const uint8_t *p;
	int r, wrapped = 0;

	mm_memset(rec, 0, sizeof(*rec));
	if (lsn <= 0 || pos < lr->first || pos >= lr->file_size || (pos & 7) ||
	    in < lr->data_ofs || in + lr->rec_hdr > lr->ps)
		return ST_NOT_FOUND;
	r = ntfs_lr_page(lr, page, &p);
	if (r != ST_OK)
		return ST_NOT_FOUND;
	/* A record of an older pass carries an older sequence number in its
	 * LSN: the log ends where the LSN found is not the one expected. */
	if ((int64_t)ntfs_le64(p + in) != lsn)
		return ST_NOT_FOUND;
	rec->lsn = lsn;
	rec->prev_lsn = (int64_t)ntfs_le64(p + in + 0x08);
	rec->undo_next_lsn = (int64_t)ntfs_le64(p + in + 0x10);
	rec->cdl = ntfs_le32(p + in + 0x18);
	rec->type = ntfs_le32(p + in + 0x20);
	rec->tx = ntfs_le32(p + in + 0x24);
	if (rec->cdl > NTFS_LR_MAX_REC || (rec->type != 1 && rec->type != 2))
		return ST_IO;
	rec->cd = (uint8_t *)kalloc(rec->cdl ? rec->cdl : 1);
	if (!rec->cd)
		return ST_NOMEM;
	total = rec->cdl;
	in += lr->rec_hdr;
	/* The client data, page by page. */
	while (got < total) {
		uint32_t n;

		if (in >= lr->ps) {
			page = ntfs_lr_next_page(lr, page, &wrapped);
			r = ntfs_lr_page(lr, page, &p);
			if (r != ST_OK) {
				kfree(rec->cd);
				rec->cd = NULL;
				return ST_IO;
			}
			in = lr->data_ofs;
		}
		n = lr->ps - (uint32_t)in;
		if (n > total - got)
			n = total - got;
		mm_memcpy(rec->cd + got, p + in, n);
		got += n;
		in += n;
	}
	/* The next record: 8-byte aligned, on the next page when the rest of
	 * this one cannot hold a record header. */
	in = (in + 7) & ~7ULL;
	if (in + lr->rec_hdr > lr->ps) {
		page = ntfs_lr_next_page(lr, page, &wrapped);
		in = lr->data_ofs;
	}
	*next = ntfs_lr_make_lsn(lr, seq + (wrapped ? 1 : 0), page + in);
	/* The operation header of an update. */
	if (rec->type == 1 && rec->cdl >= 0x20) {
		const uint8_t *h = rec->cd;

		rec->redo_op = ntfs_le16(h + 0x00);
		rec->undo_op = ntfs_le16(h + 0x02);
		rec->redo_off = ntfs_le16(h + 0x04);
		rec->redo_len = ntfs_le16(h + 0x06);
		rec->undo_off = ntfs_le16(h + 0x08);
		rec->undo_len = ntfs_le16(h + 0x0A);
		rec->target_attr = ntfs_le16(h + 0x0C);
		rec->lcns = ntfs_le16(h + 0x0E);
		rec->rec_off = ntfs_le16(h + 0x10);
		rec->attr_off = ntfs_le16(h + 0x12);
		rec->cluster_index = ntfs_le16(h + 0x14);
		rec->vcn = (int64_t)ntfs_le64(h + 0x18);
		rec->lcn_list = h + 0x20;
		if ((uint32_t)rec->lcns * 8U + 0x20U > rec->cdl ||
		    (rec->redo_len &&
		     (uint32_t)rec->redo_off + rec->redo_len > rec->cdl) ||
		    (rec->undo_len &&
		     (uint32_t)rec->undo_off + rec->undo_len > rec->cdl)) {
			kfree(rec->cd);
			rec->cd = NULL;
			return ST_IO;
		}
	}
	return ST_OK;
}

static void ntfs_lr_rec_free(ntfs_lr_rec_t *rec)
{
	ntfs_free(rec->cd);
	rec->cd = NULL;
}

/* ---- the restart area ---- */

static int ntfs_lr_restart(ntfs_lr_t *lr)
{
	ntfs_fs_t *fs = lr->fs;
	uint8_t *b = NULL;
	int64_t best = -1;
	int r = ST_IO;

	b = (uint8_t *)kalloc(2 * 65536);
	if (!b)
		return ST_NOMEM;
	for (uint32_t off = 0; off <= 65536; off = off ? off << 1 : 512) {
		const uint8_t *ra, *cl;
		uint32_t sps, ca, in_use, clients;
		uint8_t *p = b;

		if (off + 4096 > lr->log->data_size ||
		    ntfs_rl_pread(fs, &lr->log->rl, off, 4096,
				  lr->log->init_size, p) != ST_OK)
			continue;
		if (ntfs_le32(p) != NTFS_MAGIC_RSTR)
			continue;
		sps = ntfs_le32(p + 0x10);
		if (sps < 512 || sps > 65536 || !ntfs_is_pow2(sps) ||
		    (off && off != sps) || off + sps > lr->log->data_size)
			continue;
		if (sps > 4096 &&
		    ntfs_rl_pread(fs, &lr->log->rl, off, sps,
				  lr->log->init_size, p) != ST_OK)
			continue;
		if (ntfs_le16(p + 6) != sps / NTFS_BLOCK_SIZE + 1 ||
		    ntfs_mst_fixup(p, sps) != ST_OK)
			continue;
		ra = p + ntfs_le16(p + 0x18);
		if (ntfs_le16(p + 0x18) + NTFS_RESTART_AREA_HDR > sps)
			continue;
		ca = ntfs_le16(ra + 0x16);
		clients = ntfs_le16(ra + 0x08);
		in_use = ntfs_le16(ra + 0x0C);
		if (in_use >= clients ||
		    ntfs_le16(p + 0x18) + ca +
				    (uint64_t)clients * NTFS_LOG_CLIENT_RECORD >
			    sps)
			continue;
		if ((int64_t)ntfs_le64(ra) <= best)
			continue;
		best = (int64_t)ntfs_le64(ra);
		lr->sys_ps = sps;
		lr->ps = ntfs_le32(p + 0x14);
		lr->major = (int16_t)ntfs_le16(p + 0x1C);
		lr->current_lsn = (int64_t)ntfs_le64(ra);
		lr->seq_bits = ntfs_le32(ra + 0x10);
		lr->file_size = ntfs_le64(ra + 0x18);
		lr->rec_hdr = ntfs_le16(ra + 0x24);
		lr->data_ofs = ntfs_le16(ra + 0x26);
		cl = ra + ca + (uint64_t)in_use * NTFS_LOG_CLIENT_RECORD;
		lr->oldest_lsn = (int64_t)ntfs_le64(cl + 0x00);
		lr->restart_lsn = (int64_t)ntfs_le64(cl + 0x08);
		/* The client must be "NTFS". */
		if (ntfs_le32(cl + 0x1C) != 8 || ntfs_le16(cl + 0x20) != 'N' ||
		    ntfs_le16(cl + 0x22) != 'T' || ntfs_le16(cl + 0x24) != 'F' ||
		    ntfs_le16(cl + 0x26) != 'S')
			best = -2;
		r = ST_OK;
	}
	kfree(b);
	if (r != ST_OK || best < 0)
		return ntfs_lr_fail(lr, "no usable restart area");
	if (lr->ps < 512 || lr->ps > 65536 || !ntfs_is_pow2(lr->ps) ||
	    lr->rec_hdr != 0x30 || lr->data_ofs < 0x28 ||
	    lr->data_ofs >= lr->ps || (lr->data_ofs & 7) ||
	    lr->seq_bits < 16 || lr->seq_bits > 60 ||
	    ntfs_log_seq_bits(lr->file_size) != lr->seq_bits ||
	    lr->file_size > lr->log->data_size || (lr->file_size % lr->ps))
		return ntfs_lr_fail(lr, "restart area out of range");
	if (lr->major != 1 && lr->major != 2)
		return ntfs_lr_fail(lr, "unknown log version");
	lr->file_bits = 64 - lr->seq_bits;
	lr->first = lr->major >= 2 ? 0x22ULL * lr->ps :
				     2ULL * lr->sys_ps + 2ULL * lr->ps;
	if (lr->first + 2 * lr->ps > lr->file_size)
		return ntfs_lr_fail(lr, "log too small");
	return ST_OK;
}

/* ---- tables ---- */

static ntfs_lr_oat_t *ntfs_lr_oat_find(ntfs_lr_t *lr, uint32_t key)
{
	for (uint32_t i = 0; i < lr->noat; i++)
		if (lr->oat[i].key == key)
			return &lr->oat[i];
	return NULL;
}

/* Install open attribute `key' from a table entry `e' (`esz' bytes: 0x28
 * for the current layout, 0x2C for the older one). */
static int ntfs_lr_oat_set(ntfs_lr_t *lr, uint32_t key, const uint8_t *e,
			   uint32_t esz)
{
	ntfs_lr_oat_t *o = ntfs_lr_oat_find(lr, key);

	if (!o) {
		if (lr->noat >= NTFS_LR_MAX_OAT)
			return ntfs_lr_fail(lr, "too many open attributes");
		o = &lr->oat[lr->noat++];
		mm_memset(o, 0, sizeof(*o));
		o->key = key;
	}
	if (esz == 0x28) {
		o->bytes_per_index = ntfs_le32(e + 0x04);
		o->type = ntfs_le32(e + 0x08);
		o->mref = ntfs_le64(e + 0x10);
	} else if (esz == 0x2C) {
		o->mref = ntfs_le64(e + 0x08);
		o->type = ntfs_le32(e + 0x1C);
		o->bytes_per_index = ntfs_le32(e + 0x28);
	} else {
		return ntfs_lr_fail(lr, "unknown attribute table layout");
	}
	o->name_len = 0xFF; /* until a name is known (or 0 for none) */
	if (esz == 0x28 && !e[0x0D])
		o->name_len = 0; /* no name */
	if (esz == 0x2C && !e[0x19])
		o->name_len = 0;
	return ST_OK;
}

/* Walk a restart table (header 0x18 bytes, then entries); `cb' for every
 * allocated entry with its key (offset). */
static int ntfs_lr_table(ntfs_lr_t *lr, const uint8_t *t, uint32_t len,
			 int (*cb)(ntfs_lr_t *, uint32_t, const uint8_t *,
				   uint32_t))
{
	uint32_t esz, n;

	if (len < 0x18)
		return ntfs_lr_fail(lr, "short restart table");
	esz = ntfs_le16(t + 0x00);
	n = ntfs_le16(t + 0x02);
	if (esz < 8 || (uint64_t)esz * n + 0x18 > len)
		return ntfs_lr_fail(lr, "restart table out of range");
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *e = t + 0x18 + (uint64_t)esz * i;

		if (ntfs_le32(e) != NTFS_LR_RESTART_ALLOCATED)
			continue;
		if (cb(lr, 0x18 + esz * i, e, esz) != ST_OK)
			return ST_IO;
	}
	return ST_OK;
}

static int ntfs_lr_oat_cb(ntfs_lr_t *lr, uint32_t key, const uint8_t *e,
			  uint32_t esz)
{
	return ntfs_lr_oat_set(lr, key, e, esz);
}

static ntfs_lr_tx_t *ntfs_lr_tx_get(ntfs_lr_t *lr, uint32_t key, int add)
{
	for (uint32_t i = 0; i < lr->ntx; i++)
		if (lr->tx[i].key == key)
			return &lr->tx[i];
	if (!add || lr->ntx >= NTFS_LR_MAX_TX)
		return NULL;
	lr->tx[lr->ntx].key = key;
	lr->tx[lr->ntx].state = NTFS_LR_TX_ACTIVE;
	lr->tx[lr->ntx].undo_next = 0;
	return &lr->tx[lr->ntx++];
}

static int ntfs_lr_tt_cb(ntfs_lr_t *lr, uint32_t key, const uint8_t *e,
			 uint32_t esz)
{
	ntfs_lr_tx_t *t;

	if (esz < 0x28)
		return ntfs_lr_fail(lr, "unknown transaction table layout");
	t = ntfs_lr_tx_get(lr, key, 1);
	if (!t)
		return ntfs_lr_fail(lr, "too many transactions");
	t->state = e[4];
	t->undo_next = (int64_t)ntfs_le64(e + 0x18);
	if (t->state < NTFS_LR_TX_ACTIVE || t->state > NTFS_LR_TX_COMMITTED)
		return ntfs_lr_fail(lr, "unknown transaction state");
	return ST_OK;
}

/* The attribute names dump: {16-bit table offset, 16-bit length in bytes,
 * the name} entries, ended by a zero entry. */
static int ntfs_lr_names(ntfs_lr_t *lr, const uint8_t *d, uint32_t len)
{
	uint32_t p = 0;

	while (p + 4 <= len) {
		uint32_t key = ntfs_le16(d + p), nl = ntfs_le16(d + p + 2);
		ntfs_lr_oat_t *o;

		if (!key && !nl)
			break;
		if ((nl & 1) || nl > 2 * NTFS_MAX_NAME_LEN || p + 4 + nl > len)
			return ntfs_lr_fail(lr, "bad attribute name dump");
		o = ntfs_lr_oat_find(lr, key);
		if (o) {
			o->name_len = (uint8_t)(nl / 2);
			mm_memcpy(o->name, d + p + 4, nl);
		}
		p += 4 + nl + 2; /* the name is NUL-terminated */
		p = (p + 1) & ~1U;
	}
	return ST_OK;
}

/* Load the table dump record at `lsn' (operation `op'). */
static int ntfs_lr_dump(ntfs_lr_t *lr, int64_t lsn, uint32_t len, uint16_t op)
{
	ntfs_lr_rec_t rec;
	int64_t next;
	int r;

	if (!lsn || !len)
		return ST_OK;
	r = ntfs_lr_read(lr, lsn, &rec, &next);
	if (r != ST_OK)
		return ntfs_lr_fail(lr, "a table dump is not readable");
	if (rec.type != 1 || rec.redo_op != op || rec.redo_len < len) {
		ntfs_lr_rec_free(&rec);
		return ntfs_lr_fail(lr, "a table dump does not match");
	}
	if (op == NTFS_LOP_OAT_DUMP)
		r = ntfs_lr_table(lr, rec.cd + rec.redo_off, rec.redo_len,
				  ntfs_lr_oat_cb);
	else if (op == NTFS_LOP_TT_DUMP)
		r = ntfs_lr_table(lr, rec.cd + rec.redo_off, rec.redo_len,
				  ntfs_lr_tt_cb);
	else if (op == NTFS_LOP_NAMES_DUMP)
		r = ntfs_lr_names(lr, rec.cd + rec.redo_off, rec.redo_len);
	ntfs_lr_rec_free(&rec);
	return r;
}

/* ---- the overlay ---- */

static ntfs_lr_page_t *ntfs_lr_lookup(ntfs_lr_t *lr, uint64_t off)
{
	for (ntfs_lr_page_t *p = lr->hash[(off >> 9) % NTFS_LR_HASH]; p;
	     p = p->next)
		if (p->off == off)
			return p;
	return NULL;
}

/* The overlay page at volume offset `off' (`size' bytes, `kind'), read
 * from the device on first use. */
static int ntfs_lr_get(ntfs_lr_t *lr, uint64_t off, uint32_t size,
		       uint8_t kind, uint64_t mft_no, ntfs_lr_page_t **out)
{
	ntfs_fs_t *fs = lr->fs;
	ntfs_lr_page_t *p = ntfs_lr_lookup(lr, off);
	uint64_t h;

	if (p) {
		if (p->size != size || p->kind != kind)
			return ntfs_lr_fail(lr, "overlapping targets");
		*out = p;
		return ST_OK;
	}
	if ((off | size) & (fs->dev_ss - 1) || !size ||
	    off + size > (fs->nr_clusters << fs->cluster_bits) ||
	    off < ntfs_boot_bytes(fs))
		return ntfs_lr_fail(lr, "target outside the volume");
	if (lr->bytes + size > NTFS_LR_MAX_BYTES)
		return ntfs_lr_fail(lr, "the replay needs too much memory");
	p = (ntfs_lr_page_t *)ntfs_zalloc(sizeof(*p));
	if (!p)
		return ST_NOMEM;
	p->data = (uint8_t *)kalloc(size);
	if (!p->data) {
		kfree(p);
		return ST_NOMEM;
	}
	if (ntfs_dev_pread(fs, off, size, p->data) != ST_OK) {
		kfree(p->data);
		kfree(p);
		return ntfs_lr_fail(lr, "a target could not be read");
	}
	p->off = off;
	p->size = size;
	p->kind = kind;
	p->mft_no = mft_no;
	p->valid = kind == NTFS_LR_RAW ||
		   ntfs_mst_fixup(p->data, size) == ST_OK;
	h = (off >> 9) % NTFS_LR_HASH;
	p->next = lr->hash[h];
	lr->hash[h] = p;
	p->all = lr->pages;
	lr->pages = p;
	lr->bytes += size;
	*out = p;
	return ST_OK;
}

/* Where does cluster `vcn' of attribute `o' lie?  Through the volume's
 * own metadata (the attribute's runlist as read before the replay); the
 * record's LCN must agree. */
static int ntfs_lr_map(ntfs_lr_t *lr, const ntfs_lr_oat_t *o, int64_t vcn,
		       int64_t want_lcn)
{
	ntfs_runlist_t rl;
	ntfs_attr_sizes_t sz;
	int64_t lcn = -1;
	int r;

	if (o->name_len == 0xFF)
		return ntfs_lr_fail(lr, "the name of a target is unknown");
	if (NTFS_MREF_NO(o->mref) == NTFS_FILE_MFT && o->type == NTFS_AT_DATA &&
	    !o->name_len) {
		r = ntfs_rl_map(&lr->fs->st_mft->rl, vcn, &lcn, NULL);
	} else {
		r = ntfs_attr_runlist(lr->fs,
				      NTFS_MAKE_MREF(NTFS_MREF_NO(o->mref), 0),
				      o->type,
				      o->name_len ? o->name : ntfs_unnamed,
				      o->name_len, &rl, &sz);
		if (r != ST_OK)
			return ntfs_lr_fail(lr, "a target attribute is missing");
		r = sz.resident ? ST_IO : ntfs_rl_map(&rl, vcn, &lcn, NULL);
		ntfs_rl_free(&rl);
	}
	if (r != ST_OK || lcn < 0 || lcn != want_lcn)
		return ntfs_lr_fail(lr, "a target is not where the volume "
					"has it");
	return ST_OK;
}

/* The page a record targets: an MFT record, an index block, or the raw
 * clusters it lists (checked against the attribute's runlist). */
static int ntfs_lr_target(ntfs_lr_t *lr, const ntfs_lr_rec_t *rec,
			  ntfs_lr_page_t **out)
{
	ntfs_fs_t *fs = lr->fs;
	const ntfs_lr_oat_t *o = ntfs_lr_oat_find(lr, rec->target_attr);
	uint64_t aoff, voff;
	uint32_t size;
	uint8_t kind;
	uint64_t no = 0;
	int r;

	if (!o)
		return ntfs_lr_fail(lr, "a record names no open attribute");
	if (!rec->lcns || rec->vcn < 0 || rec->vcn > (1LL << 40))
		return ntfs_lr_fail(lr, "a record names no cluster");
	for (uint32_t k = 0; k < rec->lcns; k++) {
		r = ntfs_lr_map(lr, o, rec->vcn + k,
				(int64_t)ntfs_le64(rec->lcn_list + 8 * k));
		if (r != ST_OK)
			return r;
	}
	aoff = ((uint64_t)rec->vcn << fs->cluster_bits) +
	       (uint64_t)rec->cluster_index * NTFS_BLOCK_SIZE;
	voff = ((uint64_t)ntfs_le64(rec->lcn_list) << fs->cluster_bits) +
	       (uint64_t)rec->cluster_index * NTFS_BLOCK_SIZE;
	if (NTFS_MREF_NO(o->mref) == NTFS_FILE_MFT && o->type == NTFS_AT_DATA &&
	    !o->name_len) {
		kind = NTFS_LR_MFT;
		size = fs->rec_size;
		no = aoff >> fs->rec_bits;
		if (aoff & (fs->rec_size - 1) || no >= fs->mft_records)
			return ntfs_lr_fail(lr, "a record target is misplaced");
	} else if (o->type == NTFS_AT_INDEX_ALLOCATION) {
		kind = NTFS_LR_INDX;
		size = o->bytes_per_index;
		if (size < NTFS_BLOCK_SIZE || size > 65536 || !ntfs_is_pow2(size) ||
		    (aoff & (size - 1)))
			return ntfs_lr_fail(lr, "an index target is misplaced");
	} else {
		kind = NTFS_LR_RAW;
		size = (uint32_t)(rec->lcns << fs->cluster_bits) -
		       rec->cluster_index * NTFS_BLOCK_SIZE;
		if (rec->cluster_index * NTFS_BLOCK_SIZE >= fs->cluster_size)
			return ntfs_lr_fail(lr, "a data target is misplaced");
	}
	/* Records and index blocks larger than a cluster span the listed
	 * clusters, which must then be contiguous. */
	if (size > fs->cluster_size - rec->cluster_index * NTFS_BLOCK_SIZE) {
		uint64_t need = (rec->cluster_index * NTFS_BLOCK_SIZE + size +
				 fs->cluster_size - 1) >>
				fs->cluster_bits;

		if (need > rec->lcns)
			return ntfs_lr_fail(lr, "a target lacks its clusters");
		for (uint32_t k = 1; k < need; k++)
			if (ntfs_le64(rec->lcn_list + 8 * k) !=
			    ntfs_le64(rec->lcn_list) + k)
				return ntfs_lr_fail(lr, "a target is "
							"fragmented");
	}
	return ntfs_lr_get(lr, voff, size, kind, no, out);
}

/* ---- the operations ---- */

/* Bytes in use / allocated of an MFT record image. */
static uint32_t ntfs_lr_rec_used(const uint8_t *r)
{
	return ntfs_le32(r + 0x18);
}

/* Move [from, end) of buffer `b' by `delta' (positive: towards the end),
 * inside `cap' bytes. */
static int ntfs_lr_shift(uint8_t *b, uint32_t cap, uint32_t from,
			 uint32_t end, int64_t delta)
{
	if (from > end || end > cap)
		return ST_IO;
	if (delta > 0 && (uint64_t)end + (uint64_t)delta > cap)
		return ST_IO;
	if (delta < 0 && (int64_t)from + delta < 0)
		return ST_IO;
	ntfs_memmove(b + (int64_t)from + delta, b + from, end - from);
	return ST_OK;
}

/* Apply operation `op' with `data' (`dlen' bytes; `other_len': the length
 * of the opposite data -- the resident-value rule) to page `p'. */
static int ntfs_lr_apply(ntfs_lr_t *lr, ntfs_lr_page_t *p,
			 const ntfs_lr_rec_t *rec, uint16_t op,
			 const uint8_t *data, uint32_t dlen, uint32_t other_len)
{
	ntfs_fs_t *fs = lr->fs;
	uint8_t *b = p->data;
	uint32_t roff = rec->rec_off, aoff = rec->attr_off;
	uint32_t used, alen;
	uint8_t *a;

	(void)fs;
	switch (op) {
	case NTFS_LOP_NOOP:
	case NTFS_LOP_CLR:
	case NTFS_LOP_DELETE_DIRTY_CLUSTERS:
		return ST_OK;
	case NTFS_LOP_INIT_FILE_RECORD:
		if (p->kind != NTFS_LR_MFT || roff || dlen < 0x30 ||
		    dlen > p->size)
			return ntfs_lr_fail(lr, "bad record initialisation");
		mm_memset(b, 0, p->size);
		mm_memcpy(b, data, dlen);
		p->valid = 1;
		return ST_OK;
	case NTFS_LOP_DEALLOC_FILE_RECORD:
		if (p->kind != NTFS_LR_MFT || !p->valid)
			return ntfs_lr_fail(lr, "bad record release");
		ntfs_put_le16(b + 0x16, ntfs_le16(b + 0x16) &
						(uint16_t)~NTFS_MFT_REC_IN_USE);
		ntfs_put_le16(b + 0x10, (uint16_t)(ntfs_le16(b + 0x10) + 1));
		return ST_OK;
	default:
		break;
	}
	if (p->kind == NTFS_LR_MFT) {
		if (!p->valid)
			return ntfs_lr_fail(lr, "an update of a damaged record");
		used = ntfs_lr_rec_used(b);
		if (used > p->size || used < 0x38 ||
		    roff < ntfs_le16(b + 0x14) || roff >= used)
			return ntfs_lr_fail(lr, "an attribute offset is wrong");
		a = b + roff;
		alen = ntfs_le32(a + 4);
		switch (op) {
		case NTFS_LOP_WRITE_END_OF_RECORD:
			if ((uint64_t)roff + aoff + dlen > p->size)
				return ntfs_lr_fail(lr, "record overflow");
			mm_memcpy(b + roff + aoff, data, dlen);
			ntfs_put_le32(b + 0x18, roff + aoff + dlen);
			return ST_OK;
		case NTFS_LOP_CREATE_ATTRIBUTE:
			if (aoff || dlen < 0x18 || ntfs_le32(data + 4) != dlen ||
			    (dlen & 7) ||
			    ntfs_lr_shift(b, p->size, roff, used, dlen) != ST_OK)
				return ntfs_lr_fail(lr, "bad attribute creation");
			mm_memcpy(a, data, dlen);
			ntfs_put_le32(b + 0x18, used + dlen);
			if (ntfs_le16(a + 0x0E) >= ntfs_le16(b + 0x28))
				ntfs_put_le16(b + 0x28,
					      (uint16_t)(ntfs_le16(a + 0x0E) + 1));
			return ST_OK;
		case NTFS_LOP_DELETE_ATTRIBUTE:
			if (aoff || alen < 0x18 || roff + alen > used ||
			    ntfs_lr_shift(b, p->size, roff + alen, used,
					  -(int64_t)alen) != ST_OK)
				return ntfs_lr_fail(lr, "bad attribute removal");
			ntfs_put_le32(b + 0x18, used - alen);
			return ST_OK;
		case NTFS_LOP_UPDATE_RESIDENT: {
			uint32_t voff, nsz;

			if (a[8] || alen < 0x18 || roff + alen > used)
				return ntfs_lr_fail(lr, "bad resident update");
			voff = ntfs_le16(a + 0x14);
			if (aoff < voff)
				return ntfs_lr_fail(lr, "bad resident update");
			if (dlen == other_len) {
				/* In place. */
				if (aoff + dlen > alen)
					return ntfs_lr_fail(lr, "resident "
								"update overflow");
				mm_memcpy(a + aoff, data, dlen);
				return ST_OK;
			}
			/* The value now ends with this data. */
			nsz = (aoff + dlen + 7) & ~7U;
			if (ntfs_lr_shift(b, p->size, roff + alen, used,
					  (int64_t)nsz - alen) != ST_OK)
				return ntfs_lr_fail(lr, "resident update does "
							"not fit");
			mm_memcpy(a + aoff, data, dlen);
			if (nsz > aoff + dlen)
				mm_memset(a + aoff + dlen, 0, nsz - aoff - dlen);
			ntfs_put_le32(a + 4, nsz);
			ntfs_put_le32(a + 0x10, aoff + dlen - voff);
			ntfs_put_le32(b + 0x18, used + nsz - alen);
			return ST_OK;
		}
		case NTFS_LOP_UPDATE_MAPPING_PAIRS: {
			uint32_t nsz;

			if (!a[8] || roff + alen > used ||
			    aoff < ntfs_le16(a + 0x20))
				return ntfs_lr_fail(lr, "bad mapping pairs "
							"update");
			nsz = (aoff + dlen + 7) & ~7U;
			if (nsz < alen)
				nsz = alen;
			if (nsz != alen &&
			    ntfs_lr_shift(b, p->size, roff + alen, used,
					  (int64_t)nsz - alen) != ST_OK)
				return ntfs_lr_fail(lr, "mapping pairs do not "
							"fit");
			mm_memcpy(a + aoff, data, dlen);
			if (nsz != alen) {
				mm_memset(a + aoff + dlen, 0, nsz - aoff - dlen);
				ntfs_put_le32(a + 4, nsz);
				ntfs_put_le32(b + 0x18, used + nsz - alen);
			}
			return ST_OK;
		}
		case NTFS_LOP_SET_SIZES:
			if (!a[8] || alen < 0x40 || dlen < 24 || dlen > 32 ||
			    ntfs_le64(a + 0x10) /* lowest VCN */)
				return ntfs_lr_fail(lr, "bad size update");
			mm_memcpy(a + 0x28, data, 24);
			if (dlen == 32 && ntfs_le16(a + 0x20) >= 0x48 &&
			    alen >= 0x48)
				mm_memcpy(a + 0x40, data + 24, 8);
			return ST_OK;
		case NTFS_LOP_ADD_INDEX_ROOT:
		case NTFS_LOP_DELETE_INDEX_ROOT:
		case NTFS_LOP_SET_VCN_ROOT:
		case NTFS_LOP_UPDATE_NAME_ROOT:
		case NTFS_LOP_UPDATE_DATA_ROOT: {
			uint32_t voff, hdr, ilen, epos, elen;

			if (a[8] || ntfs_le32(a) != NTFS_AT_INDEX_ROOT ||
			    roff + alen > used)
				return ntfs_lr_fail(lr, "bad index root update");
			voff = ntfs_le16(a + 0x14);
			hdr = voff + NTFS_INDEX_ROOT_HDR; /* index header */
			if (hdr + NTFS_INDEX_HDR > alen)
				return ntfs_lr_fail(lr, "bad index root");
			ilen = ntfs_le32(a + hdr + 4);
			epos = aoff;
			if (epos < hdr + ntfs_le32(a + hdr) ||
			    epos + NTFS_IE_HDR > hdr + ilen || hdr + ilen > alen)
				return ntfs_lr_fail(lr, "bad index root entry");
			elen = ntfs_le16(a + epos + 8);
			if (op == NTFS_LOP_ADD_INDEX_ROOT) {
				if (dlen < NTFS_IE_HDR || ntfs_le16(data + 8) != dlen ||
				    (dlen & 7) ||
				    ntfs_lr_shift(b, p->size, roff + epos, used,
						  dlen) != ST_OK)
					return ntfs_lr_fail(lr, "index root "
								"insertion");
				mm_memcpy(a + epos, data, dlen);
				ntfs_put_le32(a + 4, alen + dlen);
				ntfs_put_le32(a + 0x10, ntfs_le32(a + 0x10) + dlen);
				ntfs_put_le32(a + hdr + 4, ilen + dlen);
				ntfs_put_le32(a + hdr + 8,
					      ntfs_le32(a + hdr + 8) + dlen);
				ntfs_put_le32(b + 0x18, used + dlen);
				return ST_OK;
			}
			if (elen < NTFS_IE_HDR || (elen & 7) ||
			    epos + elen > hdr + ilen)
				return ntfs_lr_fail(lr, "bad index root entry");
			if (op == NTFS_LOP_DELETE_INDEX_ROOT) {
				if (ntfs_lr_shift(b, p->size, roff + epos + elen,
						  used, -(int64_t)elen) != ST_OK)
					return ntfs_lr_fail(lr, "index root "
								"removal");
				ntfs_put_le32(a + 4, alen - elen);
				ntfs_put_le32(a + 0x10, ntfs_le32(a + 0x10) - elen);
				ntfs_put_le32(a + hdr + 4, ilen - elen);
				ntfs_put_le32(a + hdr + 8,
					      ntfs_le32(a + hdr + 8) - elen);
				ntfs_put_le32(b + 0x18, used - elen);
				return ST_OK;
			}
			if (op == NTFS_LOP_SET_VCN_ROOT) {
				if (dlen != 8 || !(ntfs_le16(a + epos + 12) &
						   NTFS_INDEX_ENTRY_NODE))
					return ntfs_lr_fail(lr, "bad VCN update");
				mm_memcpy(a + epos + elen - 8, data, 8);
				return ST_OK;
			}
			if (op == NTFS_LOP_UPDATE_NAME_ROOT) {
				if (dlen > 0x38 || elen < NTFS_IE_HDR + 0x42)
					return ntfs_lr_fail(lr, "bad name update");
				mm_memcpy(a + epos + NTFS_IE_HDR + 8, data, dlen);
				return ST_OK;
			}
			/* NTFS_LOP_UPDATE_DATA_ROOT: a view entry's data. */
			if ((uint32_t)ntfs_le16(a + epos) + dlen > elen)
				return ntfs_lr_fail(lr, "bad index data update");
			mm_memcpy(a + epos + ntfs_le16(a + epos), data, dlen);
			return ST_OK;
		}
		case NTFS_LOP_UPDATE_NONRESIDENT:
			/* A record written as plain data (an attribute list
			 * stored in the MFT's own data, say). */
			if ((uint64_t)roff + aoff + dlen > p->size)
				return ntfs_lr_fail(lr, "record data overflow");
			mm_memcpy(b + roff + aoff, data, dlen);
			return ST_OK;
		default:
			return ntfs_lr_fail(lr, "an MFT operation this driver "
						"does not replay");
		}
	}
	if (p->kind == NTFS_LR_INDX) {
		uint32_t ilen, ialloc, epos = roff + aoff, elen, eo;

		if (!p->valid || ntfs_le32(b) != NTFS_MAGIC_INDX)
			return ntfs_lr_fail(lr, "an update of a damaged index "
						"block");
		eo = ntfs_le32(b + NTFS_INDX_HDR);
		ilen = ntfs_le32(b + NTFS_INDX_HDR + 4);
		ialloc = ntfs_le32(b + NTFS_INDX_HDR + 8);
		if (NTFS_INDX_HDR + ialloc > p->size || ilen > ialloc ||
		    epos < NTFS_INDX_HDR + eo ||
		    epos > NTFS_INDX_HDR + ilen)
			return ntfs_lr_fail(lr, "an index entry offset is wrong");
		switch (op) {
		case NTFS_LOP_ADD_INDEX_ALLOC:
			if (dlen < NTFS_IE_HDR || ntfs_le16(data + 8) != dlen ||
			    (dlen & 7) || ilen + dlen > ialloc ||
			    ntfs_lr_shift(b, p->size, epos, NTFS_INDX_HDR + ilen,
					  dlen) != ST_OK)
				return ntfs_lr_fail(lr, "index insertion");
			mm_memcpy(b + epos, data, dlen);
			ntfs_put_le32(b + NTFS_INDX_HDR + 4, ilen + dlen);
			return ST_OK;
		case NTFS_LOP_WRITE_END_OF_INDEX:
			if (epos + dlen > NTFS_INDX_HDR + ialloc)
				return ntfs_lr_fail(lr, "index block overflow");
			mm_memcpy(b + epos, data, dlen);
			ntfs_put_le32(b + NTFS_INDX_HDR + 4,
				      epos + dlen - NTFS_INDX_HDR);
			return ST_OK;
		default:
			break;
		}
		if (epos + NTFS_IE_HDR > NTFS_INDX_HDR + ilen)
			return ntfs_lr_fail(lr, "an index entry offset is wrong");
		elen = ntfs_le16(b + epos + 8);
		if (elen < NTFS_IE_HDR || (elen & 7) ||
		    epos + elen > NTFS_INDX_HDR + ilen)
			return ntfs_lr_fail(lr, "a damaged index entry");
		switch (op) {
		case NTFS_LOP_DELETE_INDEX_ALLOC:
			if (ntfs_lr_shift(b, p->size, epos + elen,
					  NTFS_INDX_HDR + ilen,
					  -(int64_t)elen) != ST_OK)
				return ntfs_lr_fail(lr, "index removal");
			ntfs_put_le32(b + NTFS_INDX_HDR + 4, ilen - elen);
			return ST_OK;
		case NTFS_LOP_SET_VCN_ALLOC:
			if (dlen != 8 ||
			    !(ntfs_le16(b + epos + 12) & NTFS_INDEX_ENTRY_NODE))
				return ntfs_lr_fail(lr, "bad VCN update");
			mm_memcpy(b + epos + elen - 8, data, 8);
			return ST_OK;
		case NTFS_LOP_UPDATE_NAME_ALLOC:
			if (dlen > 0x38 || elen < NTFS_IE_HDR + 0x42)
				return ntfs_lr_fail(lr, "bad name update");
			mm_memcpy(b + epos + NTFS_IE_HDR + 8, data, dlen);
			return ST_OK;
		case NTFS_LOP_UPDATE_DATA_ALLOC:
			if ((uint32_t)ntfs_le16(b + epos) + dlen > elen)
				return ntfs_lr_fail(lr, "bad index data update");
			mm_memcpy(b + epos + ntfs_le16(b + epos), data, dlen);
			return ST_OK;
		default:
			return ntfs_lr_fail(lr, "an index operation this driver "
						"does not replay");
		}
	}
	/* Raw clusters of another attribute ($Bitmap, $MFT:$BITMAP, ...). */
	switch (op) {
	case NTFS_LOP_UPDATE_NONRESIDENT:
		if ((uint64_t)roff + aoff + dlen > p->size)
			return ntfs_lr_fail(lr, "data update overflow");
		mm_memcpy(b + roff + aoff, data, dlen);
		return ST_OK;
	case NTFS_LOP_SET_BITS:
	case NTFS_LOP_CLEAR_BITS: {
		uint32_t first, bits;
		uint64_t base = (uint64_t)(roff + aoff) * 8;

		if (dlen < 8)
			return ntfs_lr_fail(lr, "bad bitmap update");
		first = ntfs_le32(data);
		bits = ntfs_le32(data + 4);
		if (base + first + bits > (uint64_t)p->size * 8 || !bits)
			return ntfs_lr_fail(lr, "bitmap update overflow");
		for (uint64_t i = base + first; i < base + first + bits; i++) {
			if (op == NTFS_LOP_SET_BITS)
				b[i >> 3] |= (uint8_t)(1U << (i & 7));
			else
				b[i >> 3] &= (uint8_t)~(1U << (i & 7));
		}
		return ST_OK;
	}
	default:
		return ntfs_lr_fail(lr, "a data operation this driver does not "
					"replay");
	}
}

/* Is `op' an update of a page (as opposed to transaction bookkeeping)? */
static int ntfs_lr_page_op(uint16_t op)
{
	return (op >= NTFS_LOP_INIT_FILE_RECORD &&
		op <= NTFS_LOP_CLEAR_BITS && op != NTFS_LOP_DELETE_DIRTY_CLUSTERS) ||
	       op == NTFS_LOP_UPDATE_DATA_ROOT || op == NTFS_LOP_UPDATE_DATA_ALLOC;
}

/* Operations the replay knows at all. */
static int ntfs_lr_known(uint16_t op)
{
	return op <= NTFS_LOP_UPDATE_DATA_ALLOC && op != NTFS_LOP_HOT_FIX;
}

/* Redo (`undo' 0) or undo record `rec'. */
static int ntfs_lr_do(ntfs_lr_t *lr, const ntfs_lr_rec_t *rec, int undo)
{
	uint16_t op = undo ? rec->undo_op : rec->redo_op;
	const uint8_t *data = rec->cd + (undo ? rec->undo_off : rec->redo_off);
	uint32_t dlen = undo ? rec->undo_len : rec->redo_len;
	uint32_t other = undo ? rec->redo_len : rec->undo_len;
	ntfs_lr_page_t *p;
	int r;

	if (!ntfs_lr_known(op))
		return ntfs_lr_fail(lr, "an operation this driver does not "
					"know");
	if (!ntfs_lr_page_op(op))
		return ST_OK;
	if (!dlen)
		data = NULL;
	r = ntfs_lr_target(lr, rec, &p);
	if (r != ST_OK)
		return r;
	/* Pages with an LSN: redo only what is not on the page yet, undo
	 * only what is. */
	if (p->kind != NTFS_LR_RAW && p->valid) {
		int64_t plsn = (int64_t)ntfs_le64(p->data + 0x08);

		if (!undo && plsn >= rec->lsn)
			return ST_OK;
		if (undo && plsn < rec->lsn)
			return ST_OK;
	}
	if (dlen && !data)
		return ntfs_lr_fail(lr, "a record without its data");
	r = ntfs_lr_apply(lr, p, rec, op, data ? data : (const uint8_t *)"",
			  dlen, other);
	if (r != ST_OK)
		return r;
	if (p->kind != NTFS_LR_RAW && !undo)
		ntfs_put_le64(p->data + 0x08, (uint64_t)rec->lsn);
	p->dirty = 1;
	return ST_OK;
}

/* ---- the passes ---- */

/* Analysis of one record (from the checkpoint on). */
static int ntfs_lr_analyse(ntfs_lr_t *lr, const ntfs_lr_rec_t *rec)
{
	ntfs_lr_tx_t *t;

	if (rec->type != 1)
		return ST_OK; /* client restart records */
	if (rec->cdl < 0x20)
		return ntfs_lr_fail(lr, "a short update record");
	if (!ntfs_lr_known(rec->redo_op) || !ntfs_lr_known(rec->undo_op))
		return ntfs_lr_fail(lr, "an operation this driver does not "
					"know");
	if (rec->redo_op == NTFS_LOP_OPEN_ATTRIBUTE) {
		uint32_t esz = rec->redo_len;

		if (esz != 0x28 && esz != 0x2C)
			return ntfs_lr_fail(lr, "bad attribute opening");
		if (ntfs_lr_oat_set(lr, rec->target_attr,
				    rec->cd + rec->redo_off, esz) != ST_OK)
			return ST_IO;
		/* The undo data is the attribute's name. */
		{
			ntfs_lr_oat_t *o = ntfs_lr_oat_find(lr,
							    rec->target_attr);

			if (o && rec->undo_len && !(rec->undo_len & 1) &&
			    rec->undo_len <= 2 * NTFS_MAX_NAME_LEN) {
				o->name_len = (uint8_t)(rec->undo_len / 2);
				mm_memcpy(o->name, rec->cd + rec->undo_off,
					  rec->undo_len);
			} else if (o && !rec->undo_len && o->name_len == 0xFF) {
				o->name_len = 0;
			}
		}
		return ST_OK;
	}
	if (rec->redo_op >= NTFS_LOP_OAT_DUMP && rec->redo_op <= NTFS_LOP_TT_DUMP)
		return ST_OK;
	t = ntfs_lr_tx_get(lr, rec->tx, 1);
	if (!t)
		return ntfs_lr_fail(lr, "too many transactions");
	switch (rec->redo_op) {
	case NTFS_LOP_COMMIT:
		t->state = NTFS_LR_TX_COMMITTED;
		t->undo_next = 0;
		return ST_OK;
	case NTFS_LOP_PREPARE:
		t->state = NTFS_LR_TX_PREPARED;
		return ST_OK;
	case NTFS_LOP_FORGET:
		t->state = NTFS_LR_TX_COMMITTED;
		t->undo_next = 0;
		return ST_OK;
	case NTFS_LOP_END_TOP_LEVEL:
		t->undo_next = rec->undo_next_lsn;
		return ST_OK;
	default:
		break;
	}
	if (rec->undo_op == NTFS_LOP_CLR)
		t->undo_next = rec->undo_next_lsn; /* compensated */
	else if (rec->undo_op != NTFS_LOP_NOOP)
		t->undo_next = rec->lsn;
	return ST_OK;
}

/* Walk the log from `start' to its end, calling `fn' per record.  The end
 * is the first position that does not hold the LSN expected there.
 * *end: the last LSN seen. */
static int ntfs_lr_walk(ntfs_lr_t *lr, int64_t start,
			int (*fn)(ntfs_lr_t *, const ntfs_lr_rec_t *),
			int64_t *end)
{
	int64_t lsn = start, next = 0;
	uint32_t n = 0;

	*end = 0;
	for (;;) {
		ntfs_lr_rec_t rec;
		int r = ntfs_lr_read(lr, lsn, &rec, &next);

		if (r == ST_NOT_FOUND)
			break;
		if (r != ST_OK)
			return r == ST_NOMEM ? r : ntfs_lr_fail(lr, "a log "
								 "record is "
								 "damaged");
		r = fn(lr, &rec);
		ntfs_lr_rec_free(&rec);
		if (r != ST_OK)
			return r;
		*end = lsn;
		if (++n > NTFS_LR_MAX_RECORDS)
			return ntfs_lr_fail(lr, "the log is too long");
		if (next <= lsn)
			return ntfs_lr_fail(lr, "the log goes backwards");
		lsn = next;
	}
	if (!n)
		return ntfs_lr_fail(lr, "the log is empty after its restart "
					"point");
	return ST_OK;
}

static int ntfs_lr_redo_one(ntfs_lr_t *lr, const ntfs_lr_rec_t *rec)
{
	if (rec->type != 1 || rec->cdl < 0x20)
		return ST_OK;
	lr->records++;
	return ntfs_lr_do(lr, rec, 0);
}

/* Undo every transaction that did not commit, newest record first. */
static int ntfs_lr_undo(ntfs_lr_t *lr)
{
	for (;;) {
		ntfs_lr_tx_t *t = NULL;
		ntfs_lr_rec_t rec;
		int64_t next;
		int r;

		/* The transaction whose next record to undo is newest. */
		for (uint32_t i = 0; i < lr->ntx; i++)
			if (lr->tx[i].state != NTFS_LR_TX_COMMITTED &&
			    lr->tx[i].undo_next > 0 &&
			    (!t || lr->tx[i].undo_next > t->undo_next))
				t = &lr->tx[i];
		if (!t)
			return ST_OK;
		if (++lr->records > NTFS_LR_MAX_RECORDS)
			return ntfs_lr_fail(lr, "the undo chain is too long");
		r = ntfs_lr_read(lr, t->undo_next, &rec, &next);
		if (r != ST_OK)
			return r == ST_NOMEM ? r :
					       ntfs_lr_fail(lr, "an undo record "
								"is missing");
		if (rec.type != 1 || rec.cdl < 0x20 || rec.tx != t->key) {
			ntfs_lr_rec_free(&rec);
			return ntfs_lr_fail(lr, "a bad undo chain");
		}
		if (rec.undo_op != NTFS_LOP_CLR)
			r = ntfs_lr_do(lr, &rec, 1);
		if (r == ST_OK && rec.undo_next_lsn >= t->undo_next) {
			ntfs_lr_rec_free(&rec);
			return ntfs_lr_fail(lr, "the undo chain loops");
		}
		t->undo_next = rec.undo_next_lsn;
		ntfs_lr_rec_free(&rec);
		if (r != ST_OK)
			return r;
	}
}

/* Structural check of a replayed index block (the reader checks the rest
 * when it reads it). */
static int ntfs_lr_indx_ok(const uint8_t *b, uint32_t size)
{
	uint32_t eo = ntfs_le32(b + NTFS_INDX_HDR);
	uint32_t ilen = ntfs_le32(b + NTFS_INDX_HDR + 4);
	uint32_t ialloc = ntfs_le32(b + NTFS_INDX_HDR + 8);
	uint32_t p;

	if (ntfs_le32(b) != NTFS_MAGIC_INDX || NTFS_INDX_HDR + ialloc > size ||
	    ilen > ialloc || eo < NTFS_INDEX_HDR || eo >= ilen)
		return 0;
	for (p = eo; p + NTFS_IE_HDR <= ilen;) {
		uint32_t elen = ntfs_le16(b + NTFS_INDX_HDR + p + 8);

		if (elen < NTFS_IE_HDR || (elen & 7) || p + elen > ilen)
			return 0;
		if (ntfs_le16(b + NTFS_INDX_HDR + p + 12) & NTFS_INDEX_ENTRY_END)
			return p + elen == ilen;
		p += elen;
	}
	return 0;
}

/* Every changed page checked, then written; the device flushed. */
static int ntfs_lr_commit(ntfs_lr_t *lr)
{
	ntfs_fs_t *fs = lr->fs;
	uint8_t *prot = NULL;
	int r = ST_OK;

	for (ntfs_lr_page_t *p = lr->pages; p; p = p->all) {
		if (!p->dirty)
			continue;
		if (p->kind == NTFS_LR_MFT &&
		    ntfs_rec_verify(fs, p->data, p->mft_no) != ST_OK)
			return ntfs_lr_fail(lr, "a replayed MFT record is not "
						"valid");
		if (p->kind == NTFS_LR_INDX && !ntfs_lr_indx_ok(p->data, p->size))
			return ntfs_lr_fail(lr, "a replayed index block is not "
						"valid");
	}
	prot = (uint8_t *)kalloc(65536);
	if (!prot)
		return ST_NOMEM;
	for (ntfs_lr_page_t *p = lr->pages; r == ST_OK && p; p = p->all) {
		if (!p->dirty)
			continue;
		if (p->kind == NTFS_LR_MFT) {
			r = ntfs_mft_write(fs, p->mft_no, p->data);
		} else if (p->kind == NTFS_LR_INDX) {
			mm_memcpy(prot, p->data, p->size);
			r = ntfs_mst_protect(prot, p->size);
			if (r == ST_OK)
				r = ntfs_dev_write(fs, ntfs_byte_to_lba(fs, p->off),
						   p->size >> fs->dev_ss_bits,
						   prot);
		} else {
			r = ntfs_dev_write(fs, ntfs_byte_to_lba(fs, p->off),
					   p->size >> fs->dev_ss_bits, p->data);
		}
	}
	kfree(prot);
	if (r == ST_OK)
		r = ntfs_dev_flush(fs);
	return r;
}

static void ntfs_lr_free(ntfs_lr_t *lr)
{
	ntfs_lr_page_t *p = lr->pages;

	while (p) {
		ntfs_lr_page_t *n = p->all;

		ntfs_free(p->data);
		kfree(p);
		p = n;
	}
	ntfs_free(lr->pagebuf);
	ntfs_free(lr->tail[0]);
	ntfs_free(lr->tail[1]);
	ntfs_free(lr->oat);
	ntfs_free(lr->tx);
}

/* Replay the log of `fs' (see the section comment).  ST_OK: replayed,
 * written, the log clean on the device.  Anything else: nothing written
 * (or, after a failed device write, the mount latched read-only). */
int ntfs_log_replay(ntfs_fs_t *fs)
{
	ntfs_lr_t *lr;
	ntfs_lr_rec_t rec;
	int64_t next, redo_start, end = 0, ckpt;
	const char *dev = fs->bdev->name ? fs->bdev->name : "?";
	int r;

	if (fs->hibernated || !fs->st_logfile ||
	    (fs->st_logfile->flags & NTFS_ST_RESIDENT))
		return ST_ROFS;
	lr = (ntfs_lr_t *)ntfs_zalloc(sizeof(*lr));
	if (!lr)
		return ST_NOMEM;
	lr->fs = fs;
	lr->log = fs->st_logfile;
	lr->pagebuf_off = ~0ULL;
	r = ntfs_lr_restart(lr);
	if (r != ST_OK)
		goto out;
	lr->pagebuf = (uint8_t *)kalloc(lr->ps);
	lr->oat = (ntfs_lr_oat_t *)ntfs_zalloc(NTFS_LR_MAX_OAT *
					       sizeof(ntfs_lr_oat_t));
	lr->tx = (ntfs_lr_tx_t *)ntfs_zalloc(NTFS_LR_MAX_TX *
					     sizeof(ntfs_lr_tx_t));
	if (!lr->pagebuf || !lr->oat || !lr->tx) {
		r = ST_NOMEM;
		goto out;
	}
	/* The tail copies of version 1.1. */
	if (lr->major == 1) {
		for (int i = 0; i < 2; i++) {
			lr->tail[i] = (uint8_t *)kalloc(lr->ps);
			if (!lr->tail[i]) {
				r = ST_NOMEM;
				goto out;
			}
			if (ntfs_lr_read_page(lr, 2ULL * lr->sys_ps +
							  (uint64_t)i * lr->ps,
					      lr->tail[i]) != ST_OK) {
				kfree(lr->tail[i]);
				lr->tail[i] = NULL;
			}
		}
	}
	/* The checkpoint and its tables. */
	r = ntfs_lr_read(lr, lr->restart_lsn, &rec, &next);
	if (r != ST_OK) {
		r = ntfs_lr_fail(lr, "the restart record is missing");
		goto out;
	}
	if (rec.type != 2 || rec.cdl < 0x40) {
		ntfs_lr_rec_free(&rec);
		r = ntfs_lr_fail(lr, "the restart record is not a checkpoint");
		goto out;
	}
	ckpt = (int64_t)ntfs_le64(rec.cd + 0x08);
	{
		int64_t oat = (int64_t)ntfs_le64(rec.cd + 0x10);
		int64_t names = (int64_t)ntfs_le64(rec.cd + 0x18);
		int64_t tt = (int64_t)ntfs_le64(rec.cd + 0x28);
		uint32_t oatl = ntfs_le32(rec.cd + 0x30);
		uint32_t namesl = ntfs_le32(rec.cd + 0x34);
		uint32_t ttl = ntfs_le32(rec.cd + 0x3C);

		ntfs_lr_rec_free(&rec);
		r = ntfs_lr_dump(lr, oat, oatl, NTFS_LOP_OAT_DUMP);
		if (r == ST_OK)
			r = ntfs_lr_dump(lr, names, namesl, NTFS_LOP_NAMES_DUMP);
		if (r == ST_OK)
			r = ntfs_lr_dump(lr, tt, ttl, NTFS_LOP_TT_DUMP);
		if (r != ST_OK)
			goto out;
	}
	if (ckpt <= 0)
		ckpt = lr->restart_lsn;
	/* Analysis from the checkpoint to the end of the log. */
	r = ntfs_lr_walk(lr, ckpt, ntfs_lr_analyse, &end);
	if (r != ST_OK)
		goto out;
	if (end < lr->current_lsn) {
		r = ntfs_lr_fail(lr, "the log ends before its restart area "
				     "says");
		goto out;
	}
	lr->last_lsn = end;
	/* Redo from the oldest LSN still needed. */
	redo_start = ckpt;
	if (lr->oldest_lsn > 0 && lr->oldest_lsn < redo_start)
		redo_start = lr->oldest_lsn;
	r = ntfs_lr_walk(lr, redo_start, ntfs_lr_redo_one, &end);
	if (r != ST_OK)
		goto out;
	r = ntfs_lr_undo(lr);
	if (r != ST_OK)
		goto out;
	/* Write it all, then the clean log.  The replay's writes neither
	 * arm the mount (the log is not emptied, the dirty flag not set:
	 * after the replay the volume is consistent) nor skip the flushes
	 * between them. */
	ntfs_meta_wlock(fs);
	fs->arming = 1;
	fs->armed = 1;
	fs->ro = 0;
	r = ntfs_lr_commit(lr);
	if (r == ST_OK) {
		fs->log_current_lsn = lr->last_lsn;
		r = ntfs_logfile_write_clean(fs);
	}
	fs->arming = 0;
	fs->armed = 0;
	fs->ro = 1;
	ntfs_meta_wunlock(fs);
	if (r == ST_OK)
		kprintf("ntfs: %s: $LogFile replayed (%u records, %llu bytes "
			"of metadata)\n",
			dev, lr->records, (unsigned long long)lr->bytes);
out:
	if (r != ST_OK)
		kprintf("ntfs: %s: $LogFile not replayed: %s\n", dev,
			lr->why ? lr->why :
			r == ST_NOMEM ? "out of memory" : "error");
	ntfs_lr_free(lr);
	kfree(lr);
	return r;
}
