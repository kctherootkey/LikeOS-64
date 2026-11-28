/* LikeOS - NTFS filesystem driver
 *
 * Streams and the handle table, inodes, stat, inode flags.
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
 * S9  Streams and the handle table
 *
 * A stream is one byte sequence of the volume: (MFT reference, attribute
 * type, attribute name) -- a file's unnamed $DATA, an alternate data
 * stream, a directory's $I30 index allocation, a metadata stream.  It is
 * opened once, decoded (sizes, flags, the whole runlist across all
 * extents, a resident value copy) and given a handle h; NTFS_BID_ENC(h, 0)
 * is its chain id (ntfs.h section 1).
 *
 * Lifetime: `refs' counts users (open files, a lookup in progress, the
 * read path).  An unreferenced stream stays in the table (its pages stay
 * cached) on an LRU of at most NTFS_STREAM_KEEP; beyond that the oldest is
 * evicted: unhashed under hdl_lock (its slot marked dying so the handle is
 * not reused), then -- outside the lock -- its pages, inode-cache entry and
 * dentries are dropped, and only then the slot is freed for reuse.  Nobody
 * maps blocks of an unreferenced stream: the page cache is asked for a
 * stream's pages only by a reference holder (and the write side keeps
 * streams with dirty pages referenced), so the lock-free block mapping ops
 * (S10) never see a stream disappear under them.
 *
 * Reserved handles (pinned for the life of the mount): NTFS_HANDLE_MFT,
 * _MFT_BITMAP, _BITMAP, _LOGFILE, _SECURE_SDS.
 * =========================================================================== */

const uint16_t ntfs_i30_name[4] = { '$', 'I', '3', '0' };

static inline uint32_t ntfs_st_bucket(uint64_t no, uint32_t type)
{
	uint64_t h = (no * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)type << 7);

	return (uint32_t)(h >> 40) & (NTFS_STREAM_HASH - 1);
}

static void ntfs_stream_free(ntfs_stream_t *st)
{
	if (!st)
		return;
	ntfs_rl_free(&st->rl);
	ntfs_free(st->res_data);
	ntfs_free(st->name);
	ntfs_free(st->dl);
	ntfs_rl_free(&st->old_rl);
	kfree(st);
}

/* LRU helpers (hdl_lock held). */
void ntfs_lru_del(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	lockdep_assert_held(&fs->hdl_lock);
	if (!st->on_lru)
		return;
	st->lru_prev->lru_next = st->lru_next;
	st->lru_next->lru_prev = st->lru_prev;
	st->lru_prev = st->lru_next = NULL;
	st->on_lru = 0;
	fs->lru_count--;
	fs->lru_rl_bytes -= st->lru_bytes;
	st->lru_bytes = 0;
}

static void ntfs_lru_add(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	lockdep_assert_held(&fs->hdl_lock);
	st->lru_next = &fs->lru;
	st->lru_prev = fs->lru.lru_prev;
	fs->lru.lru_prev->lru_next = st;
	fs->lru.lru_prev = st;
	st->on_lru = 1;
	fs->lru_count++;
	st->lru_bytes = (unsigned long)st->rl.cap * sizeof(ntfs_run_t);
	fs->lru_rl_bytes += st->lru_bytes;
}

static void ntfs_st_unhash(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	ntfs_stream_t **pp = &fs->st_hash[ntfs_st_bucket(st->mft_no, st->type)];

	lockdep_assert_held(&fs->hdl_lock);
	while (*pp) {
		if (*pp == st) {
			*pp = st->hash_next;
			break;
		}
		pp = &(*pp)->hash_next;
	}
	st->hash_next = NULL;
}

/* Drop everything the shared caches hold for an evicted stream, then free
 * it and its handle.  Called without hdl_lock; the slot is DYING. */
static void ntfs_stream_retire(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	unsigned long chain = ntfs_chain(st);
	uint64_t fl;

	pagecache_invalidate_file(&fs->sb, chain);
	icache_remove(&fs->sb, chain);
	if (st->flags & NTFS_ST_INDEX)
		dcache_invalidate_dir(&fs->sb, chain);
	/* The index-block cache is keyed by the handle too: a reused handle
	 * must never find the old stream's blocks. */
	ntfs_ic_drop_stream(fs, st->handle);
	spin_lock_irqsave(&fs->hdl_lock, &fl);
	__atomic_store_n(&fs->slots[st->handle], NULL, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	ntfs_stream_free(st);
}

/* Evict unreferenced streams beyond the keep limit. */
static void ntfs_stream_trim(ntfs_fs_t *fs, uint32_t keep)
{
	for (;;) {
		ntfs_stream_t *victim = NULL;
		uint64_t fl;

		spin_lock_irqsave(&fs->hdl_lock, &fl);
		if ((fs->lru_count > keep ||
		     fs->lru_rl_bytes > NTFS_LRU_RL_BUDGET) &&
		    fs->lru.lru_next != &fs->lru) {
			victim = fs->lru.lru_next;
			ntfs_lru_del(fs, victim);
			ntfs_st_unhash(fs, victim);
			__atomic_store_n(&fs->slots[victim->handle],
					 NTFS_SLOT_DYING, __ATOMIC_RELEASE);
		}
		spin_unlock_irqrestore(&fs->hdl_lock, fl);
		if (!victim)
			return;
		ntfs_stream_retire(fs, victim);
	}
}

/* ---- giving memory back ---------------------------------------------- */

static unsigned long ntfs_shrink_count(void *arg)
{
	return ((ntfs_fs_t *)arg)->lru_count;
}

/* From the reclaim thread.  Trimming needs no lock of the filesystem's
 * beyond its own (it runs from any stream put as well). */
static unsigned long ntfs_shrink_scan(void *arg, unsigned long nr)
{
	ntfs_fs_t *fs = arg;
	uint32_t before = fs->lru_count;
	uint32_t keep = before > nr ? before - (uint32_t)nr : 0;

	ntfs_stream_trim(fs, keep);
	return before > fs->lru_count ? before - fs->lru_count : 0;
}

void ntfs_shrinker_register(ntfs_fs_t *fs)
{
	fs->shrinker.name = "ntfs-streams";
	fs->shrinker.count = ntfs_shrink_count;
	fs->shrinker.scan = ntfs_shrink_scan;
	fs->shrinker.arg = fs;
	vm_register_shrinker(&fs->shrinker);
}

void ntfs_stream_ref(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	uint64_t fl;

	spin_lock_irqsave(&fs->hdl_lock, &fl);
	if (st->refs++ == 0)
		ntfs_lru_del(fs, st);
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
}

void ntfs_stream_put(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	uint64_t fl;
	int trim = 0;

	if (!st)
		return;
	spin_lock_irqsave(&fs->hdl_lock, &fl);
	WARN_ON(st->refs <= 0);
	if (st->refs > 0 && --st->refs == 0 && !(st->flags & NTFS_ST_SYSTEM)) {
		ntfs_lru_add(fs, st);
		trim = fs->lru_count > NTFS_STREAM_KEEP ||
		       fs->lru_rl_bytes > NTFS_LRU_RL_BUDGET;
	}
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	if (trim)
		ntfs_stream_trim(fs, NTFS_STREAM_KEEP);
}

/* Look the stream up (hdl_lock held); a hit is referenced. */
ntfs_stream_t *ntfs_stream_find_locked(ntfs_fs_t *fs, uint64_t no,
				       uint16_t seq, uint32_t type,
				       const uint16_t *name,
				       uint32_t nlen, int ic)
{
	lockdep_assert_held(&fs->hdl_lock);
	for (ntfs_stream_t *st = fs->st_hash[ntfs_st_bucket(no, type)]; st;
	     st = st->hash_next) {
		if (st->mft_no != no || st->type != type ||
		    st->name_len != nlen || (seq && st->seq && st->seq != seq))
			continue;
		if (nlen && !(ic ? ntfs_upcase_eq(fs, st->name, name, nlen) :
				   kmemcmp(st->name, name, nlen * 2U) == 0))
			continue;
		/* A removed stream (or a deleted file's) stays allocated
		 * while it is referenced, but is never found again: a stream
		 * created later under the same name is a new one. */
		if (st->flags & NTFS_ST_DEAD)
			continue;
		if (st->refs++ == 0)
			ntfs_lru_del(fs, st);
		return st;
	}
	return NULL;
}

/* Find a free slot (hdl_lock held); 0 when the table is full. */
static uint32_t ntfs_slot_alloc_locked(ntfs_fs_t *fs)
{
	lockdep_assert_held(&fs->hdl_lock);
	for (uint32_t n = 0; n < NTFS_SLOTS - NTFS_HANDLE_FIRST_DYNAMIC; n++) {
		uint32_t h = fs->slot_hint + n;

		if (h >= NTFS_SLOTS)
			h = NTFS_HANDLE_FIRST_DYNAMIC +
			    (h - NTFS_SLOTS) %
				    (NTFS_SLOTS - NTFS_HANDLE_FIRST_DYNAMIC);
		if (h < NTFS_HANDLE_FIRST_DYNAMIC)
			continue;
		if (!fs->slots[h]) {
			fs->slot_hint = h + 1;
			return h;
		}
	}
	return 0;
}

/* Is the unnamed $DATA of `mref' the placeholder of a system-compressed
 * file?  Decided from the record itself, so that every opener of the
 * stream -- whatever path got it first -- sees the same answer.  Returns
 * the NTFS_SG_WOF flags to apply (0: a plain stream) or an error. */
static int ntfs_wof_sgflags(ntfs_fs_t *fs, uint64_t mref, int *sg)
{
	uint8_t *rp = NULL;
	uint32_t rlen = 0, f;
	int r;

	*sg = 0;
	r = ntfs_reparse_read(fs, mref, &rp, &rlen);
	if (r == ST_NOT_FOUND || r == ST_IO)
		return ST_OK; /* no (or an unreadable) reparse point: the
			       * file's own data is what it holds */
	if (r != ST_OK)
		return r;
	if (rlen >= 4 && ntfs_le32(rp) == NTFS_IO_REPARSE_TAG_WOF) {
		f = ntfs_wof_parse(rp, rlen);
		*sg = NTFS_SG_WOF;
		if (f != ~0U)
			*sg |= NTFS_SG_WOF_FMT(f);
	}
	kfree(rp);
	return ST_OK;
}

/* Fill `st' (key fields set) from the file's attributes. */
static int ntfs_stream_setup(ntfs_fs_t *fs, ntfs_stream_t *st, int sgflags)
{
	ntfs_attr_sizes_t sz;
	uint64_t mref = NTFS_MAKE_MREF(st->mft_no, st->seq);
	int r;

	r = ntfs_attr_runlist(fs, mref, st->type,
			      st->name_len ? st->name : ntfs_unnamed,
			      st->name_len, &st->rl, &sz);
	if (r != ST_OK)
		return r;
	if (st->type == NTFS_AT_DATA && !st->name_len &&
	    !(sgflags & NTFS_SG_WOF) && sz.data_size) {
		int wsg;

		r = ntfs_wof_sgflags(fs, mref, &wsg);
		if (r != ST_OK)
			return r;
		sgflags |= wsg;
	}
	st->data_size = sz.data_size;
	st->init_size = sz.init_size;
	st->alloc_size = sz.alloc_size;
	st->comp_size = sz.comp_size;
	st->wof_fmt = NTFS_WOF_NONE;
	if (sgflags & NTFS_SG_WOF) {
		int f = NTFS_SG_WOF_FMT_OF(sgflags);

		st->flags |= NTFS_ST_WOF;
		if (f >= NTFS_WOF_XPRESS4K && f <= NTFS_WOF_XPRESS16K) {
			/* XPRESS4K 4 KB, LZX 32 KB, XPRESS8K 8 KB,
			 * XPRESS16K 16 KB chunks. */
			static const uint32_t chunk[4] = { 4096, 32768, 8192,
							   16384 };

			st->wof_fmt = (uint8_t)f;
			st->wof_chunk = chunk[f];
		}
	}
	if ((sgflags & NTFS_SG_ENCRYPTED) || (sz.flags & NTFS_ATTR_IS_ENCRYPTED))
		st->flags |= NTFS_ST_ENCRYPTED;
	if (sz.resident) {
		uint8_t *val = NULL;
		uint32_t vlen = 0;

		st->flags |= NTFS_ST_RESIDENT;
		r = ntfs_attr_readall(fs, mref, st->type,
				      st->name_len ? st->name : ntfs_unnamed,
				      st->name_len, fs->rec_size, &val, &vlen);
		if (r != ST_OK)
			return r;
		st->res_data = val;
		st->res_len = vlen;
		st->data_size = st->init_size = st->alloc_size = vlen;
		return ST_OK;
	}
	if (sz.flags & NTFS_ATTR_IS_SPARSE)
		st->flags |= NTFS_ST_SPARSE;
	if (sz.flags & NTFS_ATTR_COMPRESSION_MASK) {
		/* LZNT1 is compression format 1, in units of 16 clusters,
		 * defined for clusters of at most 4 KB. */
		if ((sz.flags & NTFS_ATTR_COMPRESSION_MASK) !=
			    NTFS_ATTR_IS_COMPRESSED ||
		    sz.comp_unit != NTFS_COMPRESSION_UNIT ||
		    fs->cluster_size > NTFS_COMPRESS_MAX_CLUSTER) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: unsupported "
					  "compression (format 0x%x, unit %u, "
					  "cluster %u)",
				       (unsigned long long)st->mft_no,
				       sz.flags & NTFS_ATTR_COMPRESSION_MASK,
				       sz.comp_unit, fs->cluster_size);
			return ST_UNSUPPORTED;
		}
		st->flags |= NTFS_ST_COMPRESSED;
		st->comp_unit = sz.comp_unit;
		st->cu_bytes = fs->cluster_size << sz.comp_unit;
	} else if (st->flags & NTFS_ST_SPARSE) {
		/* A sparse, uncompressed attribute may carry the
		 * standard compression unit 4, which is what formatters write
		 * into sparse attributes. */
		if (sz.comp_unit != 0 && sz.comp_unit != NTFS_COMPRESSION_UNIT) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: non-standard "
					  "compression unit %u",
				       (unsigned long long)st->mft_no,
				       sz.comp_unit);
			return ST_UNSUPPORTED;
		}
	}
	/* Every byte below the data size must be described by
	 * the runlist; an unmappable fragment is an error, never a hole. */
	if (st->data_size &&
	    (uint64_t)st->rl.end_vcn < ((st->data_size + fs->cluster_size - 1) >>
					fs->cluster_bits)) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: runlist ends at VCN "
				  "%lld, short of the data size %llu",
			       (unsigned long long)st->mft_no,
			       (long long)st->rl.end_vcn,
			       (unsigned long long)st->data_size);
		return ST_IO;
	}
	/* Compressed data is read in whole units: the runlist must cover
	 * the unit that holds the last byte. */
	if ((st->flags & NTFS_ST_COMPRESSED) && st->data_size) {
		uint64_t cuc = 1ULL << st->comp_unit;
		uint64_t need = ((st->data_size + st->cu_bytes - 1) /
				 st->cu_bytes) *
				cuc;

		if ((uint64_t)st->rl.end_vcn < need) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: compressed "
					  "runlist shorter than its units",
				       (unsigned long long)st->mft_no);
			return ST_IO;
		}
	}
	return ST_OK;
}

int ntfs_index_geometry(ntfs_fs_t *fs, uint32_t ib_size, uint32_t *bits)
{
	if (!ntfs_is_pow2(ib_size) || ib_size < NTFS_BLOCK_SIZE ||
	    ib_size > 65536U)
		return ST_IO;
	/* VCNs count clusters when a block is at least a cluster,
	 * else 512-byte units, whatever the sector size. */
	*bits = ib_size >= fs->cluster_size ? fs->cluster_bits :
					      NTFS_BLOCK_SIZE_BITS;
	return ST_OK;
}

static int ntfs_index_stream_setup(ntfs_fs_t *fs, ntfs_stream_t *st)
{
	uint64_t mref = NTFS_MAKE_MREF(st->mft_no, st->seq);
	const ntfs_index_root_t *ir;
	uint8_t *root = NULL;
	uint32_t rlen = 0;
	int r;

	r = ntfs_index_root_read(fs, mref, st->name, st->name_len, &root,
				 &rlen);
	if (r != ST_OK)
		return r == ST_NOT_FOUND ? ST_NOTDIR : r;
	r = ntfs_index_root_check(fs, root, rlen, st->mft_no);
	if (r != ST_OK) {
		ntfs_free(root);
		return r;
	}
	ir = (const ntfs_index_root_t *)root;
	st->ib_size = ir->index_block_size;
	r = ntfs_index_geometry(fs, st->ib_size, &st->ib_vcn_bits);
	ntfs_free(root);
	if (r != ST_OK) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: bad index block "
				  "size %u",
			       (unsigned long long)st->mft_no, st->ib_size);
		return r;
	}
	st->flags |= NTFS_ST_INDEX;
	r = ntfs_stream_setup(fs, st, 0);
	if (r == ST_NOT_FOUND) {
		/* A small index: everything lives in the root. */
		ntfs_rl_free(&st->rl);
		st->flags |= NTFS_ST_NOALLOC;
		st->data_size = st->init_size = st->alloc_size = 0;
		return ST_OK;
	}
	if (r != ST_OK)
		return r;
	if (st->flags & (NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED |
			 NTFS_ST_ENCRYPTED)) {
		/* An index allocation is always plain non-resident. */
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: index allocation is "
				  "not a plain non-resident attribute",
			       (unsigned long long)st->mft_no);
		return ST_IO;
	}
	return ST_OK;
}

/* Get (and reference) the stream (mref, type, name).  Index allocation
 * streams are set up from the index root as well.  ST_NOT_FOUND when the
 * attribute does not exist. */
int ntfs_stream_get(ntfs_fs_t *fs, uint64_t mref, uint32_t type,
		    const uint16_t *name, uint32_t nlen, int sgflags,
		    ntfs_stream_t **out)
{
	uint64_t no = NTFS_MREF_NO(mref);
	uint16_t seq = NTFS_MREF_SEQ(mref);
	ntfs_stream_t *st, *other;
	uint64_t fl;
	uint32_t h;
	int r;

	*out = NULL;
	if (nlen > NTFS_MAX_NAME_LEN)
		return ST_NOT_FOUND;
	spin_lock_irqsave(&fs->hdl_lock, &fl);
	st = ntfs_stream_find_locked(fs, no, seq, type, name, nlen,
				     sgflags & NTFS_SG_IC);
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	if (st) {
		if (st->flags & NTFS_ST_BAD) {
			ntfs_stream_put(fs, st);
			return ST_IO;
		}
		*out = st;
		return ST_OK;
	}
	st = (ntfs_stream_t *)ntfs_zalloc(sizeof(*st));
	if (!st)
		return ST_NOMEM;
	st->mft_no = no;
	st->type = type;
	st->name_len = (uint8_t)nlen;
	if (nlen) {
		st->name = (uint16_t *)kalloc(nlen * 2U);
		if (!st->name) {
			kfree(st);
			return ST_NOMEM;
		}
		mm_memcpy(st->name, name, nlen * 2U);
	}
	/* The sequence number of the base record, for the key. */
	{
		uint8_t *rec = (uint8_t *)kalloc(fs->rec_size);

		if (!rec) {
			ntfs_stream_free(st);
			return ST_NOMEM;
		}
		r = ntfs_mft_read(fs, no, rec);
		if (r == ST_OK) {
			ntfs_mft_record_t *m = (ntfs_mft_record_t *)rec;

			if (!(m->flags & NTFS_MFT_REC_IN_USE))
				r = ST_NOT_FOUND;
			else if (seq && m->sequence_number &&
				 seq != m->sequence_number)
				r = ST_IO; /* stale reference */
			else
				st->seq = m->sequence_number;
		}
		kfree(rec);
		if (r != ST_OK) {
			ntfs_stream_free(st);
			return r;
		}
	}
	if (sgflags & NTFS_SG_IC) {
		/* The caller asked case-insensitively: find the stored
		 * spelling first so that the key is exact. */
		ntfs_actx_t *c;

		r = ntfs_actx_open(fs, NTFS_MAKE_MREF(no, st->seq), &c);
		if (r != ST_OK) {
			ntfs_stream_free(st);
			return r;
		}
		r = ntfs_actx_find(c, type, name, nlen, NTFS_FIND_IC);
		if (r == ST_OK)
			mm_memcpy(st->name, ntfs_attr_name(c->a), nlen * 2U);
		ntfs_actx_close(c);
		if (r != ST_OK) {
			ntfs_stream_free(st);
			return r;
		}
		sgflags &= ~NTFS_SG_IC;
	}
	if (type == NTFS_AT_INDEX_ALLOCATION)
		r = ntfs_index_stream_setup(fs, st);
	else
		r = ntfs_stream_setup(fs, st, sgflags);
	if (r != ST_OK) {
		ntfs_stream_free(st);
		return r;
	}
	/* The volume's own files are never written through a stream. */
	if (type != NTFS_AT_INDEX_ALLOCATION &&
	    ntfs_protected(fs, NTFS_MAKE_MREF(no, st->seq)))
		st->flags |= NTFS_ST_PROTECTED;
	st->refs = 1;
	spin_lock_irqsave(&fs->hdl_lock, &fl);
	other = ntfs_stream_find_locked(fs, no, st->seq, type, st->name, nlen,
					0);
	if (other) {
		spin_unlock_irqrestore(&fs->hdl_lock, fl);
		ntfs_stream_free(st);
		*out = other;
		return ST_OK;
	}
	h = ntfs_slot_alloc_locked(fs);
	if (!h) {
		spin_unlock_irqrestore(&fs->hdl_lock, fl);
		ntfs_stream_free(st);
		/* Make room for the next caller. */
		ntfs_stream_trim(fs, NTFS_STREAM_KEEP / 2);
		return ST_NOMEM;
	}
	st->handle = h;
	st->hash_next = fs->st_hash[ntfs_st_bucket(no, type)];
	fs->st_hash[ntfs_st_bucket(no, type)] = st;
	__atomic_store_n(&fs->slots[h], st, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	/* Nothing cached under this handle may survive from an earlier
	 * owner (retire dropped it; this is the second line of defence). */
	ntfs_ic_drop_stream(fs, h);
	*out = st;
	return ST_OK;
}

/* The stream of a directory's $I30 index (referenced). */
int ntfs_dir_stream(ntfs_fs_t *fs, uint64_t mref, ntfs_stream_t **out)
{
	return ntfs_stream_get(fs, mref, NTFS_AT_INDEX_ALLOCATION,
			       ntfs_i30_name, 4, 0, out);
}

/* Install a reserved metadata stream under its fixed handle (mount). */
void ntfs_stream_install_reserved(ntfs_fs_t *fs, ntfs_stream_t *st,
				  uint32_t handle)
{
	uint64_t fl;

	st->handle = handle;
	st->refs = 1;
	st->flags |= NTFS_ST_SYSTEM;
	spin_lock_irqsave(&fs->hdl_lock, &fl);
	__atomic_store_n(&fs->slots[handle], st, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
}

/* Set up a reserved stream for system file `no', attribute (type, name). */
int ntfs_stream_open_reserved(ntfs_fs_t *fs, uint64_t no, uint32_t type,
			      const char *aname, uint32_t handle,
			      ntfs_stream_t **out)
{
	ntfs_stream_t *st;
	uint16_t nm[8];
	uint32_t nlen = aname ? ntfs_ascii_to_utf16(aname, nm, 8) : 0;
	int r;

	*out = NULL;
	st = (ntfs_stream_t *)ntfs_zalloc(sizeof(*st));
	if (!st)
		return ST_NOMEM;
	st->mft_no = no;
	st->type = type;
	st->name_len = (uint8_t)nlen;
	if (nlen) {
		st->name = (uint16_t *)kalloc(nlen * 2U);
		if (!st->name) {
			kfree(st);
			return ST_NOMEM;
		}
		mm_memcpy(st->name, nm, nlen * 2U);
	}
	r = ntfs_stream_setup(fs, st, 0);
	if (r != ST_OK) {
		ntfs_stream_free(st);
		return r;
	}
	if (st->flags & (NTFS_ST_COMPRESSED | NTFS_ST_ENCRYPTED)) {
		ntfs_stream_free(st);
		return ST_IO;
	}
	ntfs_stream_install_reserved(fs, st, handle);
	*out = st;
	return ST_OK;
}

/* Every stream (unmount): the caches were released already. */
void ntfs_streams_release(ntfs_fs_t *fs)
{
	if (!fs->slots)
		return;
	for (uint32_t h = 1; h < NTFS_SLOTS; h++) {
		ntfs_stream_t *st = fs->slots[h];

		if (st && st != NTFS_SLOT_DYING)
			ntfs_stream_free(st);
		fs->slots[h] = NULL;
	}
	kfree(fs->slots);
	fs->slots = NULL;
	for (int i = 0; i < NTFS_STREAM_HASH; i++)
		fs->st_hash[i] = NULL;
	fs->lru.lru_next = fs->lru.lru_prev = &fs->lru;
	fs->lru_count = 0;
	fs->lru_rl_bytes = 0;
	fs->st_mft = fs->st_mftbmp = fs->st_bitmap = fs->st_logfile =
		fs->st_sds = NULL;
}

/* Is page `idx' of `st' NOFILL (ntfs.h section 2): not a plain copy of
 * device blocks? */
int ntfs_page_nofill(const ntfs_stream_t *st, uint64_t idx)
{
	uint64_t start;

	if (st->flags & (NTFS_ST_RESIDENT | NTFS_ST_COMPRESSED |
			 NTFS_ST_ENCRYPTED | NTFS_ST_WOF | NTFS_ST_BAD))
		return 1;
	/* The page holding the initialized size, when data follows it. */
	if (st->init_size < st->data_size && (st->init_size & (PAGE_SIZE - 1))) {
		start = idx * PAGE_SIZE;
		if (st->init_size >= start && st->init_size < start + PAGE_SIZE)
			return 1;
	}
	return 0;
}

/* ===========================================================================
 * S16  Inodes
 *
 * ntfs_inode_load() gathers what stat and open need from a file's
 * attributes: $STANDARD_INFORMATION (times, file attributes, security id),
 * the $FILE_NAME attributes (link count: a WIN32 + DOS pair or a DOS alias
 * counts once; directories always report 1; the parent),
 * unnamed $DATA sizes and flags, the $I30 index, the reparse tag (and for
 * WOF the validated reparse data), EA presence and the WSL EAs.
 * =========================================================================== */

int ntfs_inode_load(ntfs_fs_t *fs, uint64_t mref, ntfs_inode_t *ino)
{
	ntfs_actx_t *c;
	ntfs_mft_record_t *m;
	int r, have_si = 0, dos_only_names = 0;
	uint64_t dos_parent = 0;

	mm_memset(ino, 0, sizeof(*ino));
	ino->wof_format = ~0U;
	r = ntfs_actx_open(fs, mref, &c);
	if (r != ST_OK)
		return r;
	m = (ntfs_mft_record_t *)c->base;
	ino->mft_no = c->base_no;
	ino->seq = c->base_seq;
	ino->rec_flags = m->flags;
	for (;;) {
		ntfs_attr_record_t *a;

		r = ntfs_actx_find(c, NTFS_AT_UNUSED, NULL, 0, 0);
		if (r == ST_NOT_FOUND) {
			r = ST_OK;
			break;
		}
		if (r != ST_OK)
			break;
		a = c->a;
		switch (a->type) {
		case NTFS_AT_STANDARD_INFORMATION: {
			const ntfs_standard_information_t *si =
				(const ntfs_standard_information_t *)
					ntfs_attr_value(a);

			if (have_si)
				break;
			have_si = 1;
			ino->crtime = si->creation_time;
			ino->mtime = si->last_data_change_time;
			ino->ctime = si->last_mft_change_time;
			ino->atime = si->last_access_time;
			ino->fattr = si->file_attributes;
			if (a->data.resident.value_length >= NTFS_SI_SIZE_V3)
				ino->security_id = si->security_id;
			break;
		}
		case NTFS_AT_FILE_NAME: {
			const ntfs_file_name_t *fn =
				(const ntfs_file_name_t *)ntfs_attr_value(a);

			if (fn->file_name_type == NTFS_FILE_NAME_DOS) {
				dos_only_names++;
				if (!dos_parent)
					dos_parent = fn->parent_directory;
				break;
			}
			ino->nlink++;
			if (!ino->parent)
				ino->parent = fn->parent_directory;
			break;
		}
		case NTFS_AT_SECURITY_DESCRIPTOR:
			ino->has_sd = 1;
			break;
		case NTFS_AT_DATA:
			if (a->name_length)
				break; /* named streams are not the file */
			if (!a->non_resident) {
				ino->has_data = 1;
				ino->data_resident = 1;
				ino->data_flags = a->flags;
				ino->data_size = ino->alloc_size =
					a->data.resident.value_length;
			} else if (a->data.non_resident.lowest_vcn == 0) {
				ino->has_data = 1;
				ino->data_flags = a->flags;
				ino->data_size = (uint64_t)
					a->data.non_resident.data_size;
				ino->alloc_size = (uint64_t)
					a->data.non_resident.allocated_size;
				if (a->flags & (NTFS_ATTR_COMPRESSION_MASK |
						NTFS_ATTR_IS_SPARSE))
					ino->comp_size = (uint64_t)a->data
								 .non_resident
								 .compressed_size;
				else
					ino->comp_size = ino->alloc_size;
			}
			break;
		case NTFS_AT_INDEX_ROOT:
			if (a->name_length == 4 &&
			    kmemcmp(ntfs_attr_name(a), ntfs_i30_name, 8) == 0) {
				ino->is_dir = 1;
				if (!ino->dir_size)
					ino->dir_size =
						a->data.resident.value_length;
			}
			break;
		case NTFS_AT_INDEX_ALLOCATION:
			if (a->name_length == 4 &&
			    kmemcmp(ntfs_attr_name(a), ntfs_i30_name, 8) == 0 &&
			    a->non_resident &&
			    a->data.non_resident.lowest_vcn == 0)
				ino->dir_size = (uint64_t)
					a->data.non_resident.data_size;
			break;
		case NTFS_AT_REPARSE_POINT:
			ino->has_reparse = 1;
			if (!a->non_resident) {
				uint32_t vl = a->data.resident.value_length;

				ino->reparse_len = vl;
				if (vl >= 4)
					ino->reparse_tag =
						ntfs_le32(ntfs_attr_value(a));
			} else {
				ino->reparse_len = (uint32_t)ntfs_min64(
					(uint64_t)a->data.non_resident.data_size,
					0xFFFFFFFFULL);
			}
			break;
		case NTFS_AT_EA_INFORMATION: {
			const ntfs_ea_information_t *ei =
				(const ntfs_ea_information_t *)
					ntfs_attr_value(a);

			ino->has_ea_info = 1;
			ino->ea_query_len = ei->ea_query_length;
			break;
		}
		case NTFS_AT_EA:
			ino->has_ea = 1;
			break;
		default:
			break;
		}
	}
	ntfs_actx_close(c);
	if (r != ST_OK)
		return r;
	if (!have_si) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu has no "
				  "$STANDARD_INFORMATION",
			       (unsigned long long)ino->mft_no);
		return ST_IO;
	}
	if (!(ino->rec_flags & NTFS_MFT_REC_IS_DIRECTORY))
		ino->is_dir = 0;
	if (!ino->nlink && dos_only_names)
		ino->nlink = 1;
	if (!ino->parent)
		ino->parent = dos_parent;
	if (ino->is_dir)
		ino->nlink = 1;
	if (!ino->nlink)
		ino->nlink = 1; /* system files have no name of their own */
	/* A non-resident reparse point: its tag comes from the data. */
	if (ino->has_reparse && (!ino->reparse_tag ||
				 ino->reparse_tag == NTFS_IO_REPARSE_TAG_WOF)) {
		uint8_t *rp = NULL;
		uint32_t rlen = 0;

		r = ntfs_reparse_read(fs, NTFS_MAKE_MREF(ino->mft_no, ino->seq),
				      &rp, &rlen);
		if (r == ST_OK) {
			ino->reparse_tag = ntfs_le32(rp);
			if (ino->reparse_tag == NTFS_IO_REPARSE_TAG_WOF) {
				ino->wof_format = ntfs_wof_parse(rp, rlen);
			}
			kfree(rp);
		} else if (r != ST_NOT_FOUND) {
			return r;
		}
		r = ST_OK;
	}
	if (ino->has_ea_info && ino->has_ea) {
		r = ntfs_inode_load_eas(fs, NTFS_MAKE_MREF(ino->mft_no, ino->seq),
					ino);
		if (r != ST_OK)
			return r;
	}
	return ST_OK;
}

int ntfs_inode_is_link(const ntfs_inode_t *ino)
{
	return ino->has_reparse && ntfs_tag_is_link(ino->reparse_tag);
}

/* The file type bits. */
static uint32_t ntfs_inode_type(const ntfs_inode_t *ino)
{
	if (ino->has_reparse) {
		switch (ino->reparse_tag) {
		case NTFS_IO_REPARSE_TAG_SYMLINK:
		case NTFS_IO_REPARSE_TAG_MOUNT_POINT:
		case NTFS_IO_REPARSE_TAG_LX_SYMLINK:
			return S_IFLNK;
		case NTFS_IO_REPARSE_TAG_LX_FIFO:
			return S_IFIFO;
		case NTFS_IO_REPARSE_TAG_LX_CHR:
			return S_IFCHR;
		case NTFS_IO_REPARSE_TAG_LX_BLK:
			return S_IFBLK;
		case NTFS_IO_REPARSE_TAG_AF_UNIX:
			return S_IFSOCK;
		default:
			break;
		}
	}
	return ino->is_dir ? S_IFDIR : S_IFREG;
}

/* st_mode: the WSL mode when the file has one (verbatim, no mount
 * masks), else the mount's masks; READONLY clears the write bits of a
 * file without a WSL mode. */
uint32_t ntfs_inode_mode(ntfs_fs_t *fs, const ntfs_inode_t *ino)
{
	uint32_t type = ntfs_inode_type(ino);
	uint32_t perm;

	if (type == S_IFLNK)
		return S_IFLNK | 0777;
	if (ino->has_lxmod)
		return type | (ino->lx_mode & 07777);
	if (type == S_IFDIR)
		perm = 0777 & ~fs->opt.dmask;
	else
		perm = 0777 & ~fs->opt.fmask;
	if (type != S_IFDIR && (ino->fattr & NTFS_FILE_ATTR_READONLY))
		perm &= ~0222U;
	return type | perm;
}

/* Fill a kstat.  `depth' is the link's directory depth below the volume
 * root (for native_symlink=rel, which makes the target's length depend on
 * it). */
static int ntfs_stat_fill(ntfs_fs_t *fs, ntfs_inode_t *ino, uint32_t depth,
			  struct kstat *st)
{
	uint32_t mode = ntfs_inode_mode(fs, ino);
	uint64_t size, blocks;

	mm_memset(st, 0, sizeof(*st));
	st->st_ino = ino->mft_no;
	st->st_mode = mode;
	st->st_nlink = ino->nlink;
	st->st_uid = (ino->has_lxuid && !fs->opt.uid_set) ? ino->lx_uid :
							     fs->opt.uid;
	st->st_gid = (ino->has_lxgid && !fs->opt.gid_set) ? ino->lx_gid :
							     fs->opt.gid;
	if ((mode & S_IFMT) == S_IFLNK) {
		/* The size of a link is its target's length. */
		char *tbuf = pathbuf_get();
		long tl;

		if (!tbuf)
			return ST_NOMEM;
		tl = ntfs_link_target_ino(fs, ino, depth, tbuf, PATHBUF_SIZE);
		pathbuf_put(tbuf);
		size = tl > 0 ? (uint64_t)tl : 0;
		blocks = 0;
	} else if ((mode & S_IFMT) == S_IFDIR) {
		size = ino->dir_size;
		blocks = (ino->dir_size + 511) >> 9;
	} else {
		size = ino->data_size;
		if (ino->data_resident)
			blocks = ((ino->data_size + 7) & ~7ULL) >> 9;
		else
			blocks = ino->comp_size >> 9;
	}
	st->st_size = size;
	st->st_blocks = blocks;
	st->st_blksize = PAGE_SIZE;
	if (((mode & S_IFMT) == S_IFCHR || (mode & S_IFMT) == S_IFBLK) &&
	    ino->has_lxdev)
		st->st_rdev = kdev_encode(ino->lx_major, ino->lx_minor);
	ntfs_time_to_unix(ino->atime, &st->st_atime, &st->st_atime_nsec);
	ntfs_time_to_unix(ino->mtime, &st->st_mtime, &st->st_mtime_nsec);
	ntfs_time_to_unix(ino->ctime, &st->st_ctime, &st->st_ctime_nsec);
	return ST_OK;
}

/* ---- stat ---- */

/* An open (or cached) unnamed $DATA stream of the file runs ahead of the
 * record (S30): sizes and a pending modification time come from it. */
static void ntfs_inode_overlay(ntfs_fs_t *fs, ntfs_inode_t *ino)
{
	ntfs_stream_t *st;
	uint64_t fl;

	spin_lock_irqsave(&fs->hdl_lock, &fl);
	st = ntfs_stream_find_locked(fs, ino->mft_no, ino->seq, NTFS_AT_DATA,
				     ntfs_unnamed, 0, 0);
	spin_unlock_irqrestore(&fs->hdl_lock, fl);
	if (!st)
		return;
	if (!(st->flags & NTFS_ST_DEAD)) {
		ino->data_size = st->data_size;
		ino->alloc_size = st->alloc_size;
		ino->data_resident = !!(st->flags & NTFS_ST_RESIDENT);
		ino->comp_size = (st->flags & (NTFS_ST_SPARSE |
					       NTFS_ST_COMPRESSED)) ?
					 st->comp_size :
					 st->alloc_size +
						 ((uint64_t)st->dl_resv
						  << fs->cluster_bits);
		if (st->mtime_pend) {
			ino->mtime = st->mtime_pend;
			ino->ctime = st->mtime_pend;
		}
	}
	ntfs_stream_put(fs, st);
}

/* kstat of what `res' names (a stream reports its base file's inode with
 * its own size). */
static int ntfs_stat_res(ntfs_fs_t *fs, ntfs_res_t *res, struct kstat *st)
{
	ntfs_inode_t *ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	int r;

	if (!ino)
		return ST_NOMEM;
	r = ntfs_inode_load(fs, res->mref, ino);
	if (r == ST_OK) {
		ntfs_inode_overlay(fs, ino);
		r = ntfs_stat_fill(fs, ino, res->depth, st);
	}
	if (r == ST_OK && res->is_stream) {
		ntfs_stream_t *s;

		r = ntfs_stream_get(fs, res->mref, NTFS_AT_DATA, res->sname,
				    res->snlen,
				    fs->opt.streams == NTFS_STREAMS_WINDOWS ?
					    NTFS_SG_IC :
					    0,
				    &s);
		if (r == ST_OK) {
			st->st_mode = S_IFREG | (0777 & ~fs->opt.fmask);
			st->st_size = s->data_size;
			st->st_blocks =
				(s->flags & NTFS_ST_RESIDENT) ?
					((s->data_size + 7) & ~7ULL) >> 9 :
					((s->flags & (NTFS_ST_SPARSE |
						      NTFS_ST_COMPRESSED)) ?
						 s->comp_size :
						 s->alloc_size) >>
						9;
			st->st_nlink = 1;
			ntfs_stream_put(fs, s);
		}
	}
	kfree(ino);
	return r;
}

static int ntfs_stat_path(vfs_mount_t *m, const char *path, int follow,
			  struct kstat *st)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_res_t *res;
	int r;

	if (!fs || !path || !st)
		return ST_INVALID;
	res = (ntfs_res_t *)kalloc(sizeof(*res));
	if (!res)
		return ST_NOMEM;
	ntfs_meta_rlock(fs);
	r = ntfs_resolve(fs, path, follow, res);
	if (r == ST_OK)
		r = ntfs_stat_res(fs, res, st);
	ntfs_meta_runlock(fs);
	kfree(res);
	return r;
}

int ntfs_stat(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return ntfs_stat_path(m, path, 1, st);
}

int ntfs_lstat(vfs_mount_t *m, const char *path, struct kstat *st)
{
	return ntfs_stat_path(m, path, 0, st);
}

int ntfs_fstat(vfs_file_t *f, struct kstat *st)
{
	ntfs_file_t *nf = ntfs_nf(f);
	ntfs_fs_t *fs;
	ntfs_inode_t *ino;
	int r;

	if (!nf || !st)
		return ST_INVALID;
	fs = nf->fs;
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	if (!ino)
		return ST_NOMEM;
	ntfs_meta_rlock(fs);
	r = ntfs_inode_load(fs, nf->mref, ino);
	if (r == ST_OK)
		ntfs_inode_overlay(fs, ino);
	if (r == ST_OK)
		r = ntfs_stat_fill(fs, ino,
				   (ntfs_inode_is_link(ino) &&
				    fs->opt.native_symlink == NTFS_SYMLINK_REL) ?
					   ntfs_dir_depth(fs, ino->parent) :
					   0,
				   st);
	if (r == ST_OK && nf->is_ads && nf->st) {
		st->st_mode = nf->mode;
		st->st_size = nf->st->data_size;
		st->st_nlink = 1;
	}
	ntfs_meta_runlock(fs);
	kfree(ino);
	return r;
}

/* ---- inode flags ---- */

int ntfs_inode_flags(vfs_mount_t *m, unsigned long ino_no,
		     uint32_t *out_flags)
{
	ntfs_fs_t *fs = ntfs_fs_of(m);
	ntfs_inode_t *ino;
	int r;

	if (!out_flags)
		return ST_INVALID;
	*out_flags = 0;
	if (!fs)
		return ST_NO_DEVICE;
	ino = (ntfs_inode_t *)kalloc(sizeof(*ino));
	if (!ino)
		return ST_NOMEM;
	ntfs_meta_rlock(fs);
	r = ntfs_inode_load(fs, NTFS_MAKE_MREF(ino_no, 0), ino);
	ntfs_meta_runlock(fs);
	if (r == ST_OK) {
		/* System files below record 16 (not directories) are
		 * always immutable; sys_immutable too. */
		if (ino_no < NTFS_FILE_FIRST_USER && !ino->is_dir)
			*out_flags |= VFS_ATTR_IMMUTABLE;
		if (fs->opt.sys_immutable && ino_no < NTFS_FILE_FIRST_USER)
			*out_flags |= VFS_ATTR_IMMUTABLE;
		if (ino->has_lflags) {
			if (ino->lflags & NTFS_LFLAG_IMMUTABLE)
				*out_flags |= VFS_ATTR_IMMUTABLE;
			if (ino->lflags & NTFS_LFLAG_APPEND)
				*out_flags |= VFS_ATTR_APPEND;
		}
	}
	kfree(ino);
	return r == ST_OK ? ST_OK : ST_IO;
}
