/* LikeOS - NTFS filesystem driver
 *
 * Indexes: lookup, node validation, B+tree insertion and deletion.
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
 * S15  Indexes: node validation
 *
 * The rules for roots, INDX blocks and entries are listed in the banner
 * of S15 below (index access, the index cache, lookup and walks); the
 * inline entry accessors are in ntfs.h.
 * =========================================================================== */

static int ntfs_coll_known(uint32_t coll)
{
	switch (coll) {
	case NTFS_COLLATION_BINARY:
	case NTFS_COLLATION_FILE_NAME:
	case NTFS_COLLATION_UNICODE_STRING:
	case NTFS_COLLATION_NTOFS_ULONG:
	case NTFS_COLLATION_NTOFS_SID:
	case NTFS_COLLATION_NTOFS_SECURITY_HASH:
	case NTFS_COLLATION_NTOFS_ULONGS:
		return 1;
	default:
		return 0;
	}
}

/* Check the index header at `h' with `avail' bytes behind it. */
int ntfs_index_hdr_ok(const ntfs_index_header_t *h, uint32_t avail,
		      uint32_t min_entries)
{
	if (h->entries_offset < NTFS_INDEX_HDR || (h->entries_offset & 7) ||
	    h->entries_offset < min_entries)
		return 0;
	if (h->index_length <= h->entries_offset ||
	    h->index_length - h->entries_offset < NTFS_IE_HDR)
		return 0;
	if (h->allocated_size < h->index_length || h->allocated_size > avail ||
	    h->index_length > avail)
		return 0;
	return 1;
}

/* Validate every entry of the node with header `h'. */
int ntfs_node_entries_ok(ntfs_fs_t *fs, const ntfs_index_header_t *h,
			 uint32_t coll, uint32_t vcn_bits, int is_node,
			 uint64_t alloc_size)
{
	const uint8_t *base = (const uint8_t *)h;
	uint32_t pos = h->entries_offset;
	uint32_t end = h->index_length;
	uint32_t count = 0;

	(void)fs;
	while (pos < end) {
		const ntfs_index_entry_t *e;
		uint32_t len, room;

		if (end - pos < NTFS_IE_HDR)
			return 0;
		e = (const ntfs_index_entry_t *)(base + pos);
		len = e->length;
		if (len < NTFS_IE_HDR || (len & 7) || len > end - pos)
			return 0;
		if (!!(e->flags & NTFS_INDEX_ENTRY_NODE) != !!is_node)
			return 0;
		room = len - NTFS_IE_HDR;
		if (e->flags & NTFS_INDEX_ENTRY_NODE) {
			int64_t vcn;
			uint64_t byte;

			if (room < 8)
				return 0;
			room -= 8;
			vcn = (int64_t)ntfs_le64(base + pos + len - 8);
			/* No negative VCN, no overflow to bytes, and
			 * the block must exist. */
			if (vcn < 0 || (uint64_t)vcn > (~0ULL >> vcn_bits))
				return 0;
			byte = (uint64_t)vcn << vcn_bits;
			if (byte >= alloc_size)
				return 0;
		}
		if (e->flags & NTFS_INDEX_ENTRY_END)
			return 1;
		if (e->key_length == 0 || e->key_length > room)
			return 0;
		switch (coll) {
		case NTFS_COLLATION_FILE_NAME: {
			const ntfs_file_name_t *fn =
				(const ntfs_file_name_t *)(base + pos +
							   NTFS_IE_HDR);

			/* The name fits the KEY, not just the entry. */
			if (e->key_length < NTFS_FN_HDR ||
			    fn->file_name_length == 0 ||
			    NTFS_FN_HDR + 2U * fn->file_name_length >
				    e->key_length ||
			    fn->file_name_type > NTFS_FILE_NAME_WIN32_AND_DOS)
				return 0;
			break;
		}
		case NTFS_COLLATION_NTOFS_ULONG:
			if (e->key_length != 4)
				return 0;
			break;
		case NTFS_COLLATION_NTOFS_SECURITY_HASH:
			if (e->key_length != 8)
				return 0;
			break;
		case NTFS_COLLATION_NTOFS_ULONGS:
			if (e->key_length & 3)
				return 0;
			break;
		default:
			break;
		}
		if (coll != NTFS_COLLATION_FILE_NAME) {
			/* View index: the data must lie inside the entry. */
			uint32_t doff = e->data.vi.data_offset;
			uint32_t dlen = e->data.vi.data_length;

			if (dlen && (doff < NTFS_IE_HDR + e->key_length ||
				     doff > len || dlen > len - doff))
				return 0;
		}
		if (++count > 65536)
			return 0;
		pos += len;
	}
	return 0; /* no END entry */
}

int ntfs_index_root_check(ntfs_fs_t *fs, const uint8_t *root,
			  uint32_t rlen, uint64_t no)
{
	const ntfs_index_root_t *ir = (const ntfs_index_root_t *)root;

	(void)fs;
	if (rlen < NTFS_INDEX_ROOT_HDR + NTFS_INDEX_HDR + NTFS_IE_HDR ||
	    !ntfs_coll_known(ir->collation_rule) ||
	    !ntfs_index_hdr_ok(&ir->index, rlen - NTFS_INDEX_ROOT_HDR, 0) ||
	    (ir->index.flags & ~NTFS_LARGE_INDEX)) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt index root",
			       (unsigned long long)no);
		return ST_IO;
	}
	if (ir->type == NTFS_AT_FILE_NAME &&
	    ir->collation_rule != NTFS_COLLATION_FILE_NAME) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: $FILE_NAME index "
				  "with collation %u",
			       (unsigned long long)no, ir->collation_rule);
		return ST_IO;
	}
	return ST_OK;
}

/* ===========================================================================
 * S29  Index entries: $FILE_NAME keys and $I30 entries built for insertion
 *
 * The B+tree changes themselves are S29 (below).
 * =========================================================================== */

/* A $FILE_NAME value for `name' (nlen units) in directory `parent'. */
uint8_t *ntfs_fn_build(uint64_t parent, const uint16_t *name,
		       uint32_t nlen, uint8_t ntype, int64_t crt,
		       int64_t mt, int64_t ct, int64_t at,
		       uint64_t alloc, uint64_t data, uint32_t fattr,
		       uint32_t ea_or_tag, uint32_t *len)
{
	uint32_t l = NTFS_FN_HDR + 2U * nlen;
	uint8_t *b = (uint8_t *)ntfs_zalloc(l);
	ntfs_file_name_t *fn = (ntfs_file_name_t *)b;

	if (!b)
		return NULL;
	fn->parent_directory = parent;
	fn->creation_time = crt;
	fn->last_data_change_time = mt;
	fn->last_mft_change_time = ct;
	fn->last_access_time = at;
	fn->allocated_size = (int64_t)alloc;
	fn->data_size = (int64_t)data;
	fn->file_attributes = fattr;
	fn->type.reparse_point_tag = ea_or_tag;
	fn->file_name_length = (uint8_t)nlen;
	fn->file_name_type = ntype;
	mm_memcpy(b + NTFS_FN_HDR, name, 2U * nlen);
	*len = l;
	return b;
}

/* The $I30 entry of `fn' (fl bytes) for file `mref'. */
uint8_t *ntfs_ie_build(uint64_t mref, const uint8_t *fn, uint32_t fl)
{
	uint32_t len = (NTFS_IE_HDR + fl + 7U) & ~7U;
	uint8_t *b = (uint8_t *)ntfs_zalloc(len);
	ntfs_index_entry_t *ie = (ntfs_index_entry_t *)b;

	if (!b)
		return NULL;
	ie->data.indexed_file = mref;
	ie->length = (uint16_t)len;
	ie->key_length = (uint16_t)fl;
	mm_memcpy(b + NTFS_IE_HDR, fn, fl);
	return b;
}

/* ===========================================================================
 * S15  Indexes ($I30 directories, $Secure:$SII, ...)
 *
 * An index is an $INDEX_ROOT (resident, in the MFT record) plus, for a
 * large index, the INDX blocks of its $INDEX_ALLOCATION stream.  Every
 * node is validated as a whole the moment it is read, before any
 * entry is looked at:
 *
 *   root: value large enough for the root and index headers;
 *     entries_offset >= 16 and 8-aligned; entries_offset < index_length <=
 *     allocated_size <= the value; known collation; valid block size.
 *   INDX block: read only from a real
 *     non-resident allocation, at a VCN >= 0 whose byte offset does not
 *     overflow and lies inside the allocation; fixed up as exactly one
 *     block of the index's block size; magic "INDX"; the stored VCN equals
 *     the expected one; allocated_size + 0x18 == block size; entries_offset
 *     at least the canonical offset align8(16 + 2 * (1 + size / 512)).
 *   entries: every entry header and length inside
 *     index_length, length a multiple of 8 and >= 16 (+ 8 for a child
 *     VCN), non-END entries with a key that fits the entry; the NODE flag
 *     matches the node kind; child VCNs >= 0 and not overflowing when
 *     shifted to bytes; collation-specific key checks ($FILE_NAME keys:
 *     key_length >= 66 + 2 * name length, name length >= 1; ULONG keys
 *     exactly 4 bytes, SECURITY_HASH keys 8, view-index data inside the
 *     entry); the node ends with an END entry.
 *
 * Traversal is iterative with a depth limit of 32; index blocks
 * come from an index cache (fixed-up, validated copies keyed by the
 * allocation stream's handle and the VCN, NTFS_IC_ENTRIES, LRU).
 * =========================================================================== */

/* Collation result for a malformed key. */
#define NTFS_COLL_BAD 2

int ntfs_index_root_read(ntfs_fs_t *fs, uint64_t mref,
			 const uint16_t *name, uint32_t nlen,
			 uint8_t **root, uint32_t *rlen)
{
	return ntfs_attr_readall(fs, mref, NTFS_AT_INDEX_ROOT, name, nlen,
				 fs->rec_size, root, rlen);
}

void ntfs_index_close(ntfs_index_t *ix)
{
	if (!ix)
		return;
	if (ix->alloc && ix->own_alloc)
		ntfs_stream_put(ix->fs, ix->alloc);
	ntfs_free(ix->root);
	ntfs_free(ix->blk);
	ntfs_free(ix->blk2);
	kfree(ix);
}

/* Open index `name' of file `mref'.  `alloc' (optional) is the already
 * referenced allocation stream ($I30 of a directory, from the caller). */
int ntfs_index_open(ntfs_fs_t *fs, uint64_t mref, const uint16_t *name,
		    uint32_t nlen, ntfs_stream_t *alloc,
		    ntfs_index_t **out)
{
	const ntfs_index_root_t *ir;
	ntfs_index_t *ix;
	uint64_t no = NTFS_MREF_NO(mref);
	int r;

	*out = NULL;
	ix = (ntfs_index_t *)ntfs_zalloc(sizeof(*ix));
	if (!ix)
		return ST_NOMEM;
	ix->fs = fs;
	ix->mft_no = no;
	r = ntfs_index_root_read(fs, mref, name, nlen, &ix->root,
				 &ix->root_len);
	if (r != ST_OK)
		goto fail;
	r = ntfs_index_root_check(fs, ix->root, ix->root_len, no);
	if (r != ST_OK)
		goto fail;
	ir = (const ntfs_index_root_t *)ix->root;
	ix->collation = ir->collation_rule;
	ix->ib_size = ir->index_block_size;
	r = ntfs_index_geometry(fs, ix->ib_size, &ix->vcn_bits);
	if (r != ST_OK)
		goto fail;
	if (ir->index.flags & NTFS_LARGE_INDEX) {
		if (alloc) {
			ix->alloc = alloc;
		} else {
			r = ntfs_stream_get(fs, mref, NTFS_AT_INDEX_ALLOCATION,
					    name, nlen, 0, &ix->alloc);
			if (r != ST_OK) {
				r = ST_IO; /* never "absent" */
				goto fail;
			}
			ix->own_alloc = 1;
		}
		/* Blocks only from a real allocation. */
		if ((ix->alloc->flags & (NTFS_ST_NOALLOC | NTFS_ST_RESIDENT)) ||
		    !(ix->alloc->flags & NTFS_ST_INDEX) ||
		    ix->alloc->ib_size != ix->ib_size) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: large index "
					  "without a usable allocation",
				       (unsigned long long)no);
			r = ST_IO;
			goto fail;
		}
		ix->blk = (uint8_t *)kalloc(ix->ib_size);
		ix->blk2 = (uint8_t *)kalloc(ix->ib_size);
		if (!ix->blk || !ix->blk2) {
			r = ST_NOMEM;
			goto fail;
		}
	}
	if (!ntfs_node_entries_ok(fs, &ir->index, ix->collation, ix->vcn_bits,
				  !!(ir->index.flags & NTFS_LARGE_INDEX),
				  ix->alloc ? ix->alloc->data_size : 0)) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt index root "
				  "entries",
			       (unsigned long long)no);
		r = ST_IO;
		goto fail;
	}
	*out = ix;
	return ST_OK;
fail:
	if (alloc && ix->alloc == alloc)
		ix->alloc = NULL; /* the caller's reference */
	ntfs_index_close(ix);
	return r;
}

/* ---- index block cache ---- */

static int ntfs_ic_lookup(ntfs_fs_t *fs, uint32_t handle, int64_t vcn,
			  uint8_t *buf, uint32_t size)
{
	uint64_t fl;
	int hit = 0;

	spin_lock_irqsave(&fs->ic_lock, &fl);
	for (int i = fs->ic_hash[ntfs_ic_bucket(handle, vcn)]; i >= 0;
	     i = fs->ic[i].next) {
		if (fs->ic[i].handle == handle && fs->ic[i].vcn == vcn &&
		    fs->ic[i].size == size && fs->ic[i].data) {
			mm_memcpy(buf, fs->ic[i].data, size);
			fs->ic[i].stamp = ++fs->ic_clock;
			hit = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&fs->ic_lock, fl);
	return hit;
}

void ntfs_ic_unhash(ntfs_fs_t *fs, int e)
{
	int *pp = &fs->ic_hash[ntfs_ic_bucket(fs->ic[e].handle, fs->ic[e].vcn)];

	lockdep_assert_held(&fs->ic_lock);
	while (*pp >= 0) {
		if (*pp == e) {
			*pp = fs->ic[e].next;
			break;
		}
		pp = &fs->ic[*pp].next;
	}
	fs->ic[e].next = -1;
	fs->ic[e].handle = 0;
}

void ntfs_ic_insert(ntfs_fs_t *fs, uint32_t handle, int64_t vcn,
		    const uint8_t *buf, uint32_t size)
{
	uint8_t *spare = (uint8_t *)kalloc(size), *old = NULL;
	uint64_t fl, best = ~0ULL;
	int victim = -1;

	spin_lock_irqsave(&fs->ic_lock, &fl);
	for (int i = fs->ic_hash[ntfs_ic_bucket(handle, vcn)]; i >= 0;
	     i = fs->ic[i].next) {
		if (fs->ic[i].handle == handle && fs->ic[i].vcn == vcn) {
			spin_unlock_irqrestore(&fs->ic_lock, fl);
			ntfs_free(spare);
			return;
		}
	}
	for (int i = 0; i < NTFS_IC_ENTRIES; i++) {
		if (!fs->ic[i].handle) {
			victim = i;
			break;
		}
		if (fs->ic[i].stamp < best) {
			best = fs->ic[i].stamp;
			victim = i;
		}
	}
	if (victim < 0 || (!spare && (!fs->ic[victim].data ||
				      fs->ic[victim].size != size))) {
		spin_unlock_irqrestore(&fs->ic_lock, fl);
		ntfs_free(spare);
		return;
	}
	if (fs->ic[victim].handle)
		ntfs_ic_unhash(fs, victim);
	if (!fs->ic[victim].data || fs->ic[victim].size != size) {
		old = fs->ic[victim].data;
		fs->ic[victim].data = spare;
		spare = NULL;
	}
	mm_memcpy(fs->ic[victim].data, buf, size);
	fs->ic[victim].size = size;
	fs->ic[victim].handle = handle;
	fs->ic[victim].vcn = vcn;
	fs->ic[victim].stamp = ++fs->ic_clock;
	fs->ic[victim].next = fs->ic_hash[ntfs_ic_bucket(handle, vcn)];
	fs->ic_hash[ntfs_ic_bucket(handle, vcn)] = victim;
	spin_unlock_irqrestore(&fs->ic_lock, fl);
	ntfs_free(old);
	ntfs_free(spare);
}

/* Forget every cached block of one index stream (after changes; also
 * when a handle is reused). */
void ntfs_ic_drop_stream(ntfs_fs_t *fs, uint32_t handle)
{
	uint64_t fl;

	spin_lock_irqsave(&fs->ic_lock, &fl);
	for (int i = 0; i < NTFS_IC_ENTRIES; i++)
		if (fs->ic[i].handle == handle)
			ntfs_ic_unhash(fs, i);
	spin_unlock_irqrestore(&fs->ic_lock, fl);
}

void ntfs_ic_init(ntfs_fs_t *fs)
{
	for (int i = 0; i < NTFS_IC_ENTRIES; i++) {
		fs->ic[i].handle = 0;
		fs->ic[i].data = NULL;
		fs->ic[i].next = -1;
	}
	for (int i = 0; i < NTFS_IC_HASH; i++)
		fs->ic_hash[i] = -1;
	spinlock_init(&fs->ic_lock, "ntfs_ic");
}

void ntfs_ic_release(ntfs_fs_t *fs)
{
	for (int i = 0; i < NTFS_IC_ENTRIES; i++) {
		ntfs_free(fs->ic[i].data);
		fs->ic[i].data = NULL;
		fs->ic[i].handle = 0;
	}
}

/* Read, fix up and validate index block `vcn' into `buf'. */
static int ntfs_ib_read(ntfs_index_t *ix, int64_t vcn, uint8_t *buf)
{
	ntfs_fs_t *fs = ix->fs;
	ntfs_stream_t *a = ix->alloc;
	const ntfs_index_block_t *ib = (const ntfs_index_block_t *)buf;
	uint32_t canon;
	uint64_t off;
	int r;

	if (!a || vcn < 0 || (uint64_t)vcn > (~0ULL >> ix->vcn_bits))
		return ST_IO;
	off = (uint64_t)vcn << ix->vcn_bits;
	if (!ntfs_range_ok(off, ix->ib_size, a->data_size)) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: index block VCN "
				  "%lld outside the allocation",
			       (unsigned long long)ix->mft_no, (long long)vcn);
		return ST_IO;
	}
	if (ntfs_ic_lookup(fs, a->handle, vcn, buf, ix->ib_size))
		return ST_OK;
	r = ntfs_stream_pread(fs, a, off, ix->ib_size, buf, 0);
	if (r != ST_OK)
		return r == ST_NOMEM ? r : ST_IO;
	if (ib->magic != NTFS_MAGIC_INDX ||
	    ntfs_mst_fixup(buf, ix->ib_size) != ST_OK) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: index block VCN %lld "
				  "is not an intact INDX block",
			       (unsigned long long)ix->mft_no, (long long)vcn);
		return ST_IO;
	}
	canon = (NTFS_INDEX_HDR + 2U * (1U + ix->ib_size / NTFS_BLOCK_SIZE) +
		 7U) &
		~7U;
	if (ib->index_block_vcn != vcn ||
	    ib->index.allocated_size + NTFS_INDX_HDR != ix->ib_size ||
	    !ntfs_index_hdr_ok(&ib->index, ix->ib_size - NTFS_INDX_HDR, canon) ||
	    (ib->index.flags & ~NTFS_INDEX_NODE) ||
	    !ntfs_node_entries_ok(fs, &ib->index, ix->collation, ix->vcn_bits,
				  ib->index.flags & NTFS_INDEX_NODE,
				  a->data_size)) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt index block "
				  "VCN %lld",
			       (unsigned long long)ix->mft_no, (long long)vcn);
		return ST_IO;
	}
	ntfs_ic_insert(fs, a->handle, vcn, buf, ix->ib_size);
	return ST_OK;
}

/* Node accessors. */
static inline const ntfs_index_header_t *ntfs_root_hdr(const ntfs_index_t *ix)
{
	return &((const ntfs_index_root_t *)ix->root)->index;
}

/* Compare `k' with the key of entry `e' (<0, 0, >0, or NTFS_COLL_BAD). */
static int ntfs_ikey_cmp(ntfs_index_t *ix, const ntfs_ikey_t *k,
			 const ntfs_index_entry_t *e)
{
	const uint8_t *ek = ntfs_ie_key(e);
	uint32_t elen = e->key_length;

	switch (ix->collation) {
	case NTFS_COLLATION_FILE_NAME: {
		const ntfs_file_name_t *fn = ntfs_ie_fn(e);

		return ntfs_collate_names(ix->fs, (const uint16_t *)k->key,
					  k->len, ntfs_fn_name(fn),
					  fn->file_name_length, k->ic);
	}
	case NTFS_COLLATION_UNICODE_STRING:
		if (elen & 1)
			return NTFS_COLL_BAD;
		return ntfs_collate_names(ix->fs, (const uint16_t *)k->key,
					  k->len, (const uint16_t *)ek,
					  elen / 2, 1);
	case NTFS_COLLATION_NTOFS_ULONG: {
		uint32_t a, b;

		if (k->len != 4 || elen != 4)
			return NTFS_COLL_BAD;
		a = ntfs_le32(k->key);
		b = ntfs_le32(ek);
		return a < b ? -1 : (a > b ? 1 : 0);
	}
	case NTFS_COLLATION_NTOFS_SECURITY_HASH: {
		uint32_t ah, bh, ai, bi;

		if (k->len != 8 || elen != 8)
			return NTFS_COLL_BAD;
		ah = ntfs_le32(k->key);
		bh = ntfs_le32(ek);
		if (ah != bh)
			return ah < bh ? -1 : 1;
		ai = ntfs_le32((const uint8_t *)k->key + 4);
		bi = ntfs_le32(ek + 4);
		return ai < bi ? -1 : (ai > bi ? 1 : 0);
	}
	case NTFS_COLLATION_NTOFS_ULONGS: {
		uint32_t n = (k->len < elen ? k->len : elen) / 4;

		if ((k->len & 3) || (elen & 3))
			return NTFS_COLL_BAD;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t a = ntfs_le32((const uint8_t *)k->key + 4 * i);
			uint32_t b = ntfs_le32(ek + 4 * i);

			if (a != b)
				return a < b ? -1 : 1;
		}
		return k->len == elen ? 0 : (k->len < elen ? -1 : 1);
	}
	default: { /* BINARY, SID */
		uint32_t n = k->len < elen ? k->len : elen;
		int c = kmemcmp(k->key, ek, n);

		if (c)
			return c < 0 ? -1 : 1;
		return k->len == elen ? 0 : (k->len < elen ? -1 : 1);
	}
	}
}

/* Look `k' up.  ST_OK with a copy of the matching entry in *out (kfree it),
 * ST_NOT_FOUND, or ST_IO. */
int ntfs_index_lookup(ntfs_index_t *ix, const ntfs_ikey_t *k,
		      uint8_t **out)
{
	const ntfs_index_header_t *h = ntfs_root_hdr(ix);
	int depth = 0;

	*out = NULL;
	for (;;) {
		uint32_t pos = h->entries_offset;
		const ntfs_index_entry_t *e = NULL;

		while (pos < h->index_length) {
			int c;

			e = ntfs_ie_at(h, pos);
			if (e->flags & NTFS_INDEX_ENTRY_END)
				break;
			c = ntfs_ikey_cmp(ix, k, e);
			if (c == NTFS_COLL_BAD)
				return ST_IO;
			if (c == 0) {
				uint8_t *cp = (uint8_t *)kalloc(e->length);

				if (!cp)
					return ST_NOMEM;
				mm_memcpy(cp, e, e->length);
				*out = cp;
				return ST_OK;
			}
			if (c < 0)
				break;
			pos += e->length;
		}
		if (!e || !(e->flags & NTFS_INDEX_ENTRY_NODE))
			return ST_NOT_FOUND;
		/* The depth is checked before descending. */
		if (++depth > NTFS_INDEX_MAX_DEPTH) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: index is over "
					  "32 levels deep",
				       (unsigned long long)ix->mft_no);
			return ST_IO;
		}
		{
			int r = ntfs_ib_read(ix, ntfs_ie_child(e), ix->blk);

			if (r != ST_OK)
				return r;
		}
		h = ntfs_blk_hdr(ix->blk);
	}
}

/* Visit entries in collation order starting with the first one greater
 * than `after' (NULL: the very first).  One descent; then the found entry
 * and -- when it sits in a leaf -- the entries after it in that leaf.
 * ST_OK when something was visited, ST_EOF when no entry is greater. */
int ntfs_index_walk(ntfs_index_t *ix, const ntfs_ikey_t *after,
		    ntfs_ie_cb cb, void *arg)
{
	const ntfs_index_header_t *h = ntfs_root_hdr(ix);
	int64_t best_vcn = -2; /* -2: none, -1: the root */
	int64_t cur_vcn = -1;
	uint32_t best_pos = 0;
	int best_leaf = 0;
	int depth = 0;
	int r;

	for (;;) {
		uint32_t pos = h->entries_offset;
		const ntfs_index_entry_t *e = NULL;

		while (pos < h->index_length) {
			int c;

			e = ntfs_ie_at(h, pos);
			if (e->flags & NTFS_INDEX_ENTRY_END)
				break;
			if (!after)
				break;
			c = ntfs_ikey_cmp(ix, after, e);
			if (c == NTFS_COLL_BAD)
				return ST_IO;
			if (c < 0)
				break;
			pos += e->length;
		}
		if (!e)
			return ST_IO;
		if (!(e->flags & NTFS_INDEX_ENTRY_END)) {
			best_vcn = cur_vcn;
			best_pos = pos;
			best_leaf = !(e->flags & NTFS_INDEX_ENTRY_NODE);
		}
		if (!(e->flags & NTFS_INDEX_ENTRY_NODE))
			break;
		if (++depth > NTFS_INDEX_MAX_DEPTH) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: index is over "
					  "32 levels deep",
				       (unsigned long long)ix->mft_no);
			return ST_IO;
		}
		cur_vcn = ntfs_ie_child(e);
		r = ntfs_ib_read(ix, cur_vcn, ix->blk);
		if (r != ST_OK)
			return r;
		h = ntfs_blk_hdr(ix->blk);
	}
	if (best_vcn == -2)
		return ST_EOF;
	/* Back to the node holding the answer (a cache hit at worst). */
	if (best_vcn == -1) {
		h = ntfs_root_hdr(ix);
	} else if (best_vcn != cur_vcn) {
		r = ntfs_ib_read(ix, best_vcn, ix->blk2);
		if (r != ST_OK)
			return r;
		h = ntfs_blk_hdr(ix->blk2);
	}
	for (uint32_t pos = best_pos; pos < h->index_length;) {
		const ntfs_index_entry_t *e = ntfs_ie_at(h, pos);

		if (e->flags & NTFS_INDEX_ENTRY_END)
			break;
		r = cb(ix, e, arg);
		if (r)
			return r < 0 ? r : ST_OK;
		if (!best_leaf)
			break;
		pos += e->length;
	}
	return ST_OK;
}

/* ===========================================================================
 * S29  Index changes: B+tree insertion and deletion ($I30, view indexes)
 *
 * An index change runs as a SESSION (ntfs_ixs_t) on one index of one file
 * (a directory's $I30, or a view index such as $Extend\$Reparse:$R): the
 * nodes it touches are decoded into memory (the root from $INDEX_ROOT,
 * blocks through the validated reader, S15), changed there, and written
 * when the session commits.
 *
 * Node layout: entries in collation order, each with the child VCN of the
 * subtree of smaller keys when the node is internal, then the END entry
 * whose child holds the keys greater than every entry.
 *
 * Insertion: descend to the leaf, insert; a node that overflows splits
 * around its median entry, which moves up into the parent: it keeps the
 * left half's (old) block as its child, and the parent slot that pointed
 * to the old block now points to the new right block.  The root
 * grows in place while the directory's record has room (root_cap, from the
 * record's free space); beyond that its entries move into a new block and
 * the root keeps only an END entry pointing there (the tree grows a level:
 * $INDEX_ALLOCATION and $BITMAP are created on first use, a block
 * never receives more than fits).
 *
 * Deletion: an entry in a leaf is removed; an entry in an internal node is
 * replaced by its predecessor (the last entry of the rightmost leaf of its
 * left subtree), which is removed from that leaf.  A leaf left empty is
 * freed: the parent's separator for it is removed and inserted again from
 * the root (it is greater than the left subtree and smaller than the
 * right one, so the ordinary descent finds its place) -- that is the
 * merge; a chain of nodes left with nothing but an END entry collapses
 * the same way, and a root left with a single child takes that child's
 * entries back when they fit (the tree shrinks a level).  An index whose
 * blocks are all free loses $INDEX_ALLOCATION and $BITMAP again.
 *
 * Block allocation: the $BITMAP:$I30 bit of the block (8-byte granular
 * value, resident while small, copy-on-write non-resident when large);
 * the allocation grows by one block (or one cluster when blocks are
 * smaller than clusters: several blocks per cluster, VCNs in 512-byte
 * units).
 *
 * Commit order (no journal): new blocks first (unreferenced until the
 * record says so); then, when blocks were taken, the record with the
 * allocation and the bitmap marking them in use (bits this session
 * cleared stay set for now, the root unchanged); then the changed old
 * blocks bottom-up, a device flush after each level (a node that loses
 * entries -- a split child, the leaf whose predecessor moves up --
 * reaches the device before the node that gains them); the record last
 * (the root, cleared bits, the owner's times).  A power cut leaves at
 * worst an entry that is missing (a name a check puts back), never one
 * that is there twice.  The separators of leaves a deletion freed are
 * inserted again in a second session after the first is committed
 * (ntfs_ixs_delete_commit()): there the node that gains a key can be the
 * deeper one.  A block freed in a session is not reused in it.  Index
 * blocks are protected by update sequence arrays like records: a torn
 * block write is detected by the reader.
 * =========================================================================== */

#define NTFS_IXS_PATH (NTFS_INDEX_MAX_DEPTH + 2)

/* Bytes of an encoded node's entries + END entry. */
static uint32_t ntfs_xnode_bytes(const ntfs_xnode_t *nd)
{
	uint32_t b = NTFS_IE_HDR + (nd->is_node ? 8U : 0U);

	for (uint32_t i = 0; i < nd->n; i++)
		b += ((const ntfs_index_entry_t *)nd->ents[i].e)->length +
		     (nd->is_node ? 8U : 0U);
	return b;
}

/* Entries offset of an index block (canonical). */
static uint32_t ntfs_ib_eo(uint32_t ib_size)
{
	return (NTFS_INDEX_HDR + 2U * (1U + ib_size / NTFS_BLOCK_SIZE) + 7U) &
	       ~7U;
}

static uint32_t ntfs_ib_cap(uint32_t ib_size)
{
	return ib_size - NTFS_INDX_HDR - ntfs_ib_eo(ib_size);
}

static int ntfs_xnode_overflows(const ntfs_ixs_t *s, const ntfs_xnode_t *nd)
{
	uint32_t b = ntfs_xnode_bytes(nd);

	if (nd->vcn < 0)
		return NTFS_INDEX_ROOT_HDR + NTFS_INDEX_HDR + b > s->root_cap;
	return b > ntfs_ib_cap(s->ix.ib_size);
}

static void ntfs_xnode_free(ntfs_xnode_t *nd)
{
	for (uint32_t i = 0; i < nd->n; i++)
		ntfs_free(nd->ents[i].e);
	ntfs_free(nd->ents);
	kfree(nd);
}

static ntfs_xnode_t *ntfs_xnode_new(ntfs_ixs_t *s, int64_t vcn)
{
	ntfs_xnode_t *nd = (ntfs_xnode_t *)ntfs_zalloc(sizeof(*nd));

	if (!nd)
		return NULL;
	nd->vcn = vcn;
	nd->end_child = -1;
	nd->usn = 1;
	nd->next = s->nodes;
	s->nodes = nd;
	return nd;
}

/* Insert entry (e, child) at position `pos' of `nd' (ownership of `e'
 * passes, also on failure). */
static int ntfs_xnode_ins(ntfs_xnode_t *nd, uint32_t pos, uint8_t *e,
			  int64_t child)
{
	if (nd->n == nd->cap) {
		uint32_t nc = nd->cap ? nd->cap * 2 : 16;
		ntfs_ient_t *ne;

		if (nc > 65536) {
			kfree(e);
			return ST_NOSPC;
		}
		ne = (ntfs_ient_t *)kalloc(nc * sizeof(*ne));
		if (!ne) {
			kfree(e);
			return ST_NOMEM;
		}
		if (nd->n)
			mm_memcpy(ne, nd->ents, nd->n * sizeof(*ne));
		ntfs_free(nd->ents);
		nd->ents = ne;
		nd->cap = nc;
	}
	for (uint32_t i = nd->n; i > pos; i--)
		nd->ents[i] = nd->ents[i - 1];
	nd->ents[pos].e = e;
	nd->ents[pos].child = child;
	nd->n++;
	nd->dirty = 1;
	return ST_OK;
}

/* Take entry `pos' out of `nd' (the caller owns the returned entry). */
static ntfs_ient_t ntfs_xnode_take(ntfs_xnode_t *nd, uint32_t pos)
{
	ntfs_ient_t t = nd->ents[pos];

	for (uint32_t i = pos + 1; i < nd->n; i++)
		nd->ents[i - 1] = nd->ents[i];
	nd->n--;
	nd->dirty = 1;
	return t;
}

/* Decode the node with header `h' into `nd'. */
static int ntfs_xnode_decode(ntfs_xnode_t *nd, const ntfs_index_header_t *h,
			     int is_node)
{
	const uint8_t *base = (const uint8_t *)h;

	nd->is_node = is_node;
	for (uint32_t pos = h->entries_offset; pos < h->index_length;) {
		const ntfs_index_entry_t *ie =
			(const ntfs_index_entry_t *)(base + pos);
		uint32_t len = ie->length - (is_node ? 8U : 0U);
		uint8_t *cp;
		int r;

		if (ie->flags & NTFS_INDEX_ENTRY_END) {
			nd->end_child = is_node ? ntfs_ie_child(ie) : -1;
			return ST_OK;
		}
		cp = (uint8_t *)kalloc(len);
		if (!cp)
			return ST_NOMEM;
		mm_memcpy(cp, ie, len);
		((ntfs_index_entry_t *)cp)->length = (uint16_t)len;
		((ntfs_index_entry_t *)cp)->flags &=
			(uint16_t)~(NTFS_INDEX_ENTRY_NODE | NTFS_INDEX_ENTRY_END);
		r = ntfs_xnode_ins(nd, nd->n, cp, is_node ? ntfs_ie_child(ie) : -1);
		if (r != ST_OK)
			return r;
		pos += ie->length;
	}
	return ST_IO; /* validated nodes always end with an END entry */
}

/* Encode the entries of `nd' at `out' (enough room checked by the
 * caller); returns the bytes written. */
static uint32_t ntfs_xnode_encode(const ntfs_xnode_t *nd, uint8_t *out)
{
	uint32_t pos = 0;

	for (uint32_t i = 0; i < nd->n; i++) {
		const ntfs_index_entry_t *src =
			(const ntfs_index_entry_t *)nd->ents[i].e;
		ntfs_index_entry_t *d = (ntfs_index_entry_t *)(out + pos);

		mm_memcpy(d, src, src->length);
		if (nd->is_node) {
			d->length = (uint16_t)(src->length + 8);
			d->flags |= NTFS_INDEX_ENTRY_NODE;
			ntfs_put_le32(out + pos + src->length,
				      (uint32_t)nd->ents[i].child);
			ntfs_put_le32(out + pos + src->length + 4,
				      (uint32_t)((uint64_t)nd->ents[i].child >>
						 32));
		}
		pos += d->length;
	}
	{
		ntfs_index_entry_t *d = (ntfs_index_entry_t *)(out + pos);

		mm_memset(d, 0, NTFS_IE_HDR);
		d->length = (uint16_t)(NTFS_IE_HDR + (nd->is_node ? 8 : 0));
		d->flags = NTFS_INDEX_ENTRY_END |
			   (nd->is_node ? NTFS_INDEX_ENTRY_NODE : 0);
		if (nd->is_node) {
			ntfs_put_le32(out + pos + NTFS_IE_HDR,
				      (uint32_t)nd->end_child);
			ntfs_put_le32(out + pos + NTFS_IE_HDR + 4,
				      (uint32_t)((uint64_t)nd->end_child >> 32));
		}
		pos += d->length;
	}
	return pos;
}

void ntfs_ixs_free(ntfs_ixs_t *s)
{
	if (!s)
		return;
	while (s->nodes) {
		ntfs_xnode_t *nd = s->nodes;

		s->nodes = nd->next;
		ntfs_xnode_free(nd);
	}
	if (s->alloc)
		ntfs_stream_put(s->fs, s->alloc);
	ntfs_free(s->ix.blk);
	ntfs_free(s->ix.blk2);
	ntfs_free(s->bmp);
	ntfs_free(s->bmp0);
	for (uint32_t i = 0; i < s->nre; i++)
		ntfs_free(s->reins[i]);
	ntfs_free(s->reins);
	ntfs_rl_free(&s->alloc_old_rl);
	ntfs_free(s->newruns);
	kfree(s);
}

/* Undo what an aborted session allocated (nothing of it was written to a
 * referenced place). */
void ntfs_ixs_abort(ntfs_ixs_t *s)
{
	if (!s)
		return;
	if (s->alloc_changed && s->alloc) {
		ntfs_stream_t *a = s->alloc;

		ntfs_rl_free(&a->rl);
		a->rl = s->alloc_old_rl;
		mm_memset(&s->alloc_old_rl, 0, sizeof(s->alloc_old_rl));
		a->data_size = a->init_size = a->alloc_size =
			s->alloc_old_size;
		a->flags = s->alloc_old_flags;
		pagecache_invalidate_range(&s->fs->sb, ntfs_chain(a),
					   s->alloc_old_size);
	}
	for (uint32_t i = 0; i < s->nnew; i++)
		ntfs_clusters_free(s->fs, s->newruns[i].lcn,
				   (uint64_t)s->newruns[i].len);
	s->nnew = 0;
	if (s->alloc)
		ntfs_ic_drop_stream(s->fs, s->alloc->handle);
	ntfs_ixs_free(s);
}

/* Open a session on index (name, nlen) of file `mref'. */
int ntfs_ixs_open(ntfs_fs_t *fs, uint64_t mref, const uint16_t *name,
		  uint32_t nlen, ntfs_ixs_t **out)
{
	ntfs_ixs_t *s;
	uint8_t *root = NULL, *rec = NULL;
	uint32_t rlen = 0;
	const ntfs_index_root_t *ir;
	int r;

	*out = NULL;
	ntfs_assert_meta_excl(fs);
	if (nlen > 8)
		return ST_INVALID;
	s = (ntfs_ixs_t *)ntfs_zalloc(sizeof(*s));
	if (!s)
		return ST_NOMEM;
	s->fs = fs;
	s->mref = mref;
	mm_memcpy(s->name, name, 2U * nlen);
	s->nlen = nlen;
	r = ntfs_stream_get(fs, mref, NTFS_AT_INDEX_ALLOCATION, name, nlen, 0,
			    &s->alloc);
	if (r != ST_OK)
		goto fail;
	r = ntfs_index_root_read(fs, mref, name, nlen, &root, &rlen);
	if (r == ST_OK)
		r = ntfs_index_root_check(fs, root, rlen, NTFS_MREF_NO(mref));
	if (r != ST_OK)
		goto fail;
	ir = (const ntfs_index_root_t *)root;
	mm_memcpy(s->ir_hdr, root, NTFS_INDEX_ROOT_HDR);
	s->ix.fs = fs;
	s->ix.mft_no = NTFS_MREF_NO(mref);
	s->ix.collation = ir->collation_rule;
	s->ix.ib_size = ir->index_block_size;
	r = ntfs_index_geometry(fs, s->ix.ib_size, &s->ix.vcn_bits);
	if (r != ST_OK)
		goto fail;
	if (!(s->alloc->flags & NTFS_ST_NOALLOC))
		s->ix.alloc = s->alloc;
	s->had_alloc = !(s->alloc->flags & NTFS_ST_NOALLOC);
	s->ix.blk = (uint8_t *)kalloc(s->ix.ib_size);
	if (!s->ix.blk) {
		r = ST_NOMEM;
		goto fail;
	}
	if (((ir->index.flags & NTFS_LARGE_INDEX) != 0) != s->had_alloc ||
	    !ntfs_node_entries_ok(fs, &ir->index, s->ix.collation,
				  s->ix.vcn_bits,
				  !!(ir->index.flags & NTFS_LARGE_INDEX),
				  s->had_alloc ? s->alloc->data_size : 0)) {
		r = ntfs_fs_error(fs, NTFS_MREF_NO(mref), "corrupt index root");
		goto fail;
	}
	s->root = ntfs_xnode_new(s, -1);
	if (!s->root) {
		r = ST_NOMEM;
		goto fail;
	}
	r = ntfs_xnode_decode(s->root, &ir->index,
			      !!(ir->index.flags & NTFS_LARGE_INDEX));
	if (r != ST_OK)
		goto fail;
	s->root->dirty = 0;
	/* How large the root may grow: the record's free space (no growth
	 * when the record already needs an attribute list). */
	rec = (uint8_t *)kalloc(fs->rec_size);
	if (!rec) {
		r = ST_NOMEM;
		goto fail;
	}
	r = ntfs_mft_read(fs, NTFS_MREF_NO(mref), rec);
	if (r != ST_OK)
		goto fail;
	{
		const ntfs_mft_record_t *m = (const ntfs_mft_record_t *)rec;
		uint32_t free = fs->rec_size - m->bytes_in_use;
		int has_al = ntfs_rec_attr(fs, rec, NTFS_MREF_NO(mref),
					   NTFS_AT_ATTRIBUTE_LIST, 0, 0,
					   0) != NULL;

		/* Room kept for $INDEX_ALLOCATION + $BITMAP when the root
		 * first pushes down. */
		if (has_al || free < 160)
			s->root_cap = rlen;
		else
			s->root_cap = rlen + free - 160;
		if (s->root_cap > fs->rec_size / 2 && s->root_cap > rlen)
			s->root_cap = rlen > fs->rec_size / 2 ? rlen :
								fs->rec_size / 2;
	}
	kfree(rec);
	kfree(root);
	*out = s;
	return ST_OK;
fail:
	ntfs_free(rec);
	ntfs_free(root);
	ntfs_ixs_free(s);
	return r;
}

/* The node at `vcn' (from the session, or read and decoded). */
static int ntfs_ixs_node(ntfs_ixs_t *s, int64_t vcn, uint32_t depth,
			 ntfs_xnode_t **out)
{
	ntfs_xnode_t *nd;
	const ntfs_index_block_t *ib;
	int r;

	for (nd = s->nodes; nd; nd = nd->next)
		if (nd->vcn == vcn && !nd->freed) {
			*out = nd;
			return ST_OK;
		}
	if (!s->ix.alloc)
		return ntfs_fs_error(s->fs, s->ix.mft_no,
				     "index child without an allocation");
	r = ntfs_ib_read(&s->ix, vcn, s->ix.blk);
	if (r != ST_OK)
		return r == ST_NOMEM ? r :
				       ntfs_fs_error(s->fs, s->ix.mft_no,
						     "unreadable index block");
	ib = (const ntfs_index_block_t *)s->ix.blk;
	nd = ntfs_xnode_new(s, vcn);
	if (!nd)
		return ST_NOMEM;
	nd->lsn = ib->lsn;
	nd->usn = ntfs_le16(s->ix.blk + ib->usa_ofs);
	nd->depth = depth;
	r = ntfs_xnode_decode(nd, &ib->index,
			      !!(ib->index.flags & NTFS_INDEX_NODE));
	nd->dirty = 0;
	if (r != ST_OK)
		return r;
	*out = nd;
	return ST_OK;
}

/* The comparison key of entry `e' (header + key). */
static void ntfs_ixs_key(const ntfs_ixs_t *s, const uint8_t *e, ntfs_ikey_t *k)
{
	const ntfs_index_entry_t *ie = (const ntfs_index_entry_t *)e;

	k->ic = 0;
	if (s->ix.collation == NTFS_COLLATION_FILE_NAME) {
		const ntfs_file_name_t *fn = ntfs_ie_fn(ie);

		k->key = ntfs_fn_name(fn);
		k->len = fn->file_name_length;
	} else if (s->ix.collation == NTFS_COLLATION_UNICODE_STRING) {
		k->key = ntfs_ie_key(ie);
		k->len = ie->key_length / 2;
	} else {
		k->key = ntfs_ie_key(ie);
		k->len = ie->key_length;
	}
}

/* Mark a block of the index in use / free in $BITMAP. */
static int ntfs_ixs_bmp_load(ntfs_ixs_t *s)
{
	uint8_t *v = NULL;
	uint32_t vl = 0;
	int r;

	if (s->bmp_loaded)
		return ST_OK;
	r = ntfs_attr_readall(s->fs, s->mref, NTFS_AT_BITMAP, s->name, s->nlen,
			      NTFS_READALL_MAX, &v, &vl);
	if (r == ST_NOT_FOUND) {
		v = NULL;
		vl = 0;
		r = ST_OK;
	} else if (r == ST_OK) {
		s->had_bmp = 1;
	}
	if (r != ST_OK)
		return r;
	if (vl) {
		s->bmp0 = (uint8_t *)kalloc(vl);
		if (!s->bmp0) {
			ntfs_free(v);
			return ST_NOMEM;
		}
		mm_memcpy(s->bmp0, v, vl);
	}
	s->bmp0_len = vl;
	s->bmp = v;
	s->bmp_len = vl;
	s->bmp_loaded = 1;
	return ST_OK;
}

static int ntfs_ixs_bmp_bit(ntfs_ixs_t *s, uint64_t b, int set)
{
	if (b / 8 >= s->bmp_len) {
		uint32_t nl = (uint32_t)(((b / 8) + 8) & ~7ULL);
		uint8_t *nb;

		if (!set)
			return ST_OK;
		if (nl > NTFS_READALL_MAX)
			return ST_NOSPC;
		nb = (uint8_t *)ntfs_zalloc(nl);
		if (!nb)
			return ST_NOMEM;
		if (s->bmp_len)
			mm_memcpy(nb, s->bmp, s->bmp_len);
		ntfs_free(s->bmp);
		s->bmp = nb;
		s->bmp_len = nl;
	}
	if (set)
		s->bmp[b / 8] |= (uint8_t)(1U << (b & 7));
	else
		s->bmp[b / 8] &= (uint8_t)~(1U << (b & 7));
	s->bmp_dirty = 1;
	return ST_OK;
}

/* A new block: a free bit, or the allocation grows. */
static int ntfs_ixs_alloc_block(ntfs_ixs_t *s, uint32_t depth,
				ntfs_xnode_t **out)
{
	ntfs_fs_t *fs = s->fs;
	ntfs_stream_t *a = s->alloc;
	uint32_t ib = s->ix.ib_size;
	uint64_t nblocks, b;
	ntfs_xnode_t *nd;
	int r;

	r = ntfs_ixs_bmp_load(s);
	if (r != ST_OK)
		return r;
	nblocks = (a->flags & NTFS_ST_NOALLOC) ? 0 : a->data_size / ib;
	for (b = 0; b < nblocks; b++) {
		int used = b / 8 < s->bmp_len &&
			   ((s->bmp[b / 8] >> (b & 7)) & 1);
		int in_session = 0;

		if (used)
			continue;
		/* A block of the session -- also one freed in it: its old
		 * parent may still name it on the device until the commit,
		 * so it is not written with other keys before then. */
		for (ntfs_xnode_t *nd2 = s->nodes; nd2; nd2 = nd2->next)
			if (nd2->vcn == (int64_t)((b * ib) >> s->ix.vcn_bits))
				in_session = 1;
		if (!in_session)
			break;
	}
	if (b == nblocks) {
		/* Grow the allocation: one block, or a cluster of blocks. */
		uint64_t grow = ib >= fs->cluster_size ? ib : fs->cluster_size;
		uint64_t cl = grow >> fs->cluster_bits;
		ntfs_run_t *runs = NULL;
		uint32_t nr = 0;
		int64_t goal = -1;

		if (!s->alloc_changed) {
			r = ntfs_rl_copy(&s->alloc_old_rl, &a->rl);
			if (r != ST_OK)
				return r;
			s->alloc_old_size = (a->flags & NTFS_ST_NOALLOC) ?
						    0 :
						    a->alloc_size;
			s->alloc_old_flags = a->flags;
		}
		if (a->rl.count && a->rl.runs[a->rl.count - 1].lcn >= 0)
			goal = a->rl.runs[a->rl.count - 1].lcn +
			       a->rl.runs[a->rl.count - 1].len;
		/* One run per block: a block written as one request cannot
		 * be torn by a power cut into old and new sectors. */
		r = ntfs_clusters_alloc(fs, goal, cl,
					NTFS_AL_META |
						(cl > 1 ? NTFS_AL_CONTIG : 0),
					&runs, &nr);
		if (r == ST_NOSPC && cl > 1)
			r = ntfs_clusters_alloc(fs, goal, cl, NTFS_AL_META,
						&runs, &nr);
		if (r != ST_OK)
			return r;
		for (uint32_t i = 0; i < nr; i++) {
			if (r == ST_OK)
				r = ntfs_runs_add(&s->newruns, &s->nnew,
						  &s->cnew, runs[i].lcn,
						  runs[i].len);
			if (r == ST_OK)
				r = ntfs_rl_append(&a->rl, a->rl.end_vcn,
						   runs[i].lcn, runs[i].len);
		}
		if (r != ST_OK) {
			/* Not in newruns yet: give them back here. */
			for (uint32_t i = 0; i < nr; i++) {
				int noted = 0;

				for (uint32_t k = 0; k < s->nnew; k++)
					if (s->newruns[k].lcn <= runs[i].lcn &&
					    runs[i].lcn < s->newruns[k].lcn +
								  s->newruns[k].len)
						noted = 1;
				if (!noted)
					ntfs_clusters_free(fs, runs[i].lcn,
							   (uint64_t)runs[i].len);
			}
			ntfs_free(runs);
			s->alloc_changed = 1;
			return r;
		}
		ntfs_free(runs);
		if (a->flags & NTFS_ST_NOALLOC) {
			a->flags &= ~NTFS_ST_NOALLOC;
			a->data_size = a->init_size = a->alloc_size = 0;
		}
		a->data_size += grow;
		a->init_size = a->data_size;
		a->alloc_size = (uint64_t)a->rl.end_vcn << fs->cluster_bits;
		s->alloc_changed = 1;
		s->ix.alloc = a;
		b = nblocks;
	}
	r = ntfs_ixs_bmp_bit(s, b, 1);
	if (r != ST_OK)
		return r;
	nd = ntfs_xnode_new(s, (int64_t)((b * ib) >> s->ix.vcn_bits));
	if (!nd)
		return ST_NOMEM;
	nd->is_new = 1;
	nd->dirty = 1;
	nd->depth = depth;
	*out = nd;
	return ST_OK;
}

/* A failure (the bitmap could not be loaded) fails the session: the
 * block would stay marked in use with nothing referring to it. */
static int ntfs_ixs_free_block(ntfs_ixs_t *s, ntfs_xnode_t *nd)
{
	uint64_t b = ((uint64_t)nd->vcn << s->ix.vcn_bits) / s->ix.ib_size;
	int r;

	if (nd->vcn < 0)
		return ST_OK;
	r = ntfs_ixs_bmp_load(s);
	if (r != ST_OK)
		return r;
	nd->freed = 1;
	ntfs_ixs_bmp_bit(s, b, 0);
	ntfs_ic_forget(s->fs, s->alloc->handle, nd->vcn);
	return ST_OK;
}

/* Descend for key `k' recording the path; *found: the key is at
 * (path[*depth], pos[*depth]). */
static int ntfs_ixs_search(ntfs_ixs_t *s, const ntfs_ikey_t *k,
			   ntfs_xnode_t **path, uint32_t *pos,
			   uint32_t *depth, int *found)
{
	ntfs_xnode_t *nd = s->root;
	uint32_t d = 0;

	*found = 0;
	for (;;) {
		uint32_t i;
		int64_t child;
		int r;

		for (i = 0; i < nd->n; i++) {
			int c = ntfs_ikey_cmp(&s->ix, k,
					      (const ntfs_index_entry_t *)
						      nd->ents[i].e);

			if (c == NTFS_COLL_BAD)
				return ntfs_fs_error(s->fs, s->ix.mft_no,
						     "malformed index key");
			if (c == 0) {
				*found = 1;
				break;
			}
			if (c < 0)
				break;
		}
		path[d] = nd;
		pos[d] = i;
		*depth = d;
		if (*found || !nd->is_node)
			return ST_OK;
		if (d + 1 >= NTFS_IXS_PATH - 1)
			return ntfs_fs_error(s->fs, s->ix.mft_no,
					     "index too deep");
		child = i < nd->n ? nd->ents[i].child : nd->end_child;
		if (child < 0)
			return ntfs_fs_error(s->fs, s->ix.mft_no,
					     "index node without a child");
		r = ntfs_ixs_node(s, child, d + 1, &nd);
		if (r != ST_OK)
			return r;
		d++;
	}
}

/* Repair overflows from path[d] upwards (splits, root push-down). */
static int ntfs_ixs_fixup(ntfs_ixs_t *s, ntfs_xnode_t **path, uint32_t *pos,
			  uint32_t d)
{
	for (;;) {
		ntfs_xnode_t *nd = path[d], *right, *b;
		uint32_t total, acc, m;
		ntfs_ient_t mid;
		int r;

		if (!ntfs_xnode_overflows(s, nd))
			return ST_OK;
		if (d == 0) {
			/* The root moves its entries into a new block. */
			r = ntfs_ixs_alloc_block(s, 1, &b);
			if (r != ST_OK)
				return r;
			b->is_node = nd->is_node;
			b->ents = nd->ents;
			b->n = nd->n;
			b->cap = nd->cap;
			b->end_child = nd->end_child;
			nd->ents = NULL;
			nd->n = nd->cap = 0;
			nd->is_node = 1;
			nd->end_child = b->vcn;
			nd->dirty = 1;
			s->root_dirty = 1;
			for (ntfs_xnode_t *x = s->nodes; x; x = x->next)
				if (x != nd && x != b && x->depth >= 1)
					x->depth++;
			if (d + 2 >= NTFS_IXS_PATH)
				return ST_NOSPC;
			for (uint32_t k = NTFS_IXS_PATH - 1; k > 1; k--) {
				path[k] = path[k - 1];
				pos[k] = pos[k - 1];
			}
			path[0] = nd;
			pos[0] = 0;
			path[1] = b;
			d = 1;
			continue;
		}
		if (nd->n < 2)
			return ST_NOSPC; /* one entry larger than a block */
		/* Median by bytes. */
		total = ntfs_xnode_bytes(nd);
		acc = 0;
		for (m = 0; m < nd->n - 1; m++) {
			acc += ((const ntfs_index_entry_t *)nd->ents[m].e)
				       ->length +
			       (nd->is_node ? 8U : 0U);
			if (acc * 2 >= total)
				break;
		}
		if (m >= nd->n - 1)
			m = nd->n - 2;
		if (m == 0 && nd->n > 2)
			m = 1;
		r = ntfs_ixs_alloc_block(s, nd->depth, &right);
		if (r != ST_OK)
			return r;
		right->is_node = nd->is_node;
		for (uint32_t i = m + 1; i < nd->n; i++) {
			r = ntfs_xnode_ins(right, right->n, nd->ents[i].e,
					   nd->ents[i].child);
			nd->ents[i].e = NULL;
			if (r != ST_OK)
				return r;
		}
		right->end_child = nd->end_child;
		mid = nd->ents[m];
		nd->n = m;
		nd->end_child = nd->is_node ? mid.child : -1;
		nd->dirty = 1;
		/* The median points to the left (old) block, the slot
		 * that pointed to the old block now to the new one. */
		{
			ntfs_xnode_t *p = path[d - 1];
			uint32_t ppos = pos[d - 1];

			if (ppos < p->n)
				p->ents[ppos].child = right->vcn;
			else
				p->end_child = right->vcn;
			r = ntfs_xnode_ins(p, ppos, mid.e, nd->vcn);
			if (r != ST_OK)
				return r;
			if (d - 1 == 0)
				s->root_dirty = 1;
		}
		d--;
	}
}

/* Insert entry `e' (header + key [+ data], no child VCN; ownership
 * passes).  ST_EXISTS when the key is there already. */
int ntfs_ixs_insert(ntfs_ixs_t *s, uint8_t *e)
{
	ntfs_xnode_t *path[NTFS_IXS_PATH];
	uint32_t pos[NTFS_IXS_PATH], d;
	ntfs_ikey_t k;
	int found, r;

	ntfs_ixs_key(s, e, &k);
	r = ntfs_ixs_search(s, &k, path, pos, &d, &found);
	if (r != ST_OK || found) {
		kfree(e);
		return r != ST_OK ? r : ST_EXISTS;
	}
	r = ntfs_xnode_ins(path[d], pos[d], e, -1);
	if (r != ST_OK)
		return r;
	if (d == 0)
		s->root_dirty = 1;
	return ntfs_ixs_fixup(s, path, pos, d);
}

/* A leaf at path[d] has no entries left: free it and move its separator
 * (see the section comment).  `reins' collects the separators to insert
 * again. */
static int ntfs_ixs_empty_leaf(ntfs_ixs_t *s, ntfs_xnode_t **path,
			       uint32_t *pos, uint32_t d, uint8_t **reins,
			       uint32_t *nre)
{
	while (d > 0) {
		ntfs_xnode_t *leaf = path[d], *q = path[d - 1];
		uint32_t sl = pos[d - 1];
		ntfs_ient_t sep;
		int r = ntfs_ixs_free_block(s, leaf);

		if (r != ST_OK)
			return r;
		if (sl < q->n) {
			sep = ntfs_xnode_take(q, sl);
			reins[(*nre)++] = sep.e;
		} else if (q->n > 0) {
			sep = ntfs_xnode_take(q, q->n - 1);
			q->end_child = sep.child;
			reins[(*nre)++] = sep.e;
		} else {
			/* `q' had nothing but this child: it is an empty leaf
			 * now itself. */
			q->is_node = 0;
			q->end_child = -1;
			q->dirty = 1;
			if (d - 1 == 0)
				s->root_dirty = 1;
			d--;
			continue;
		}
		if (d - 1 == 0)
			s->root_dirty = 1;
		return ST_OK;
	}
	return ST_OK;
}

/* Remove the entry with key `k' (its indexed file must be `mref' for a
 * $FILE_NAME index, when mref is non-zero). */
static int ntfs_ixs_delete(ntfs_ixs_t *s, const ntfs_ikey_t *k, uint64_t mref)
{
	ntfs_xnode_t *path[NTFS_IXS_PATH];
	uint32_t pos[NTFS_IXS_PATH], d, dn;
	uint8_t **reins;
	uint32_t nre = 0;
	ntfs_xnode_t *nd;
	int found, r;

	s->deleting = 1;
	r = ntfs_ixs_search(s, k, path, pos, &d, &found);
	if (r != ST_OK)
		return r;
	if (!found)
		return ST_NOT_FOUND;
	/* Separators to insert again (heap: the stack frame stays small). */
	reins = (uint8_t **)kalloc(NTFS_IXS_PATH * 2 * sizeof(uint8_t *));
	if (!reins)
		return ST_NOMEM;
	nd = path[d];
	if (mref && s->ix.collation == NTFS_COLLATION_FILE_NAME &&
	    ((const ntfs_index_entry_t *)nd->ents[pos[d]].e)->data.indexed_file !=
		    mref) {
		const ntfs_index_entry_t *ie =
			(const ntfs_index_entry_t *)nd->ents[pos[d]].e;

		if (NTFS_MREF_NO(ie->data.indexed_file) != NTFS_MREF_NO(mref)) {
			kfree(reins);
			return ntfs_fs_error(s->fs, s->ix.mft_no,
					     "index entry names another file");
		}
	}
	if (!nd->is_node) {
		ntfs_ient_t t = ntfs_xnode_take(nd, pos[d]);

		ntfs_free(t.e);
		if (d == 0)
			s->root_dirty = 1;
		if (d > 0 && nd->n == 0)
			r = ntfs_ixs_empty_leaf(s, path, pos, d, reins, &nre);
	} else {
		/* Internal: the predecessor takes its place. */
		ntfs_xnode_t *leaf;
		ntfs_ikey_t pk;
		uint8_t *pred;
		int64_t child = nd->ents[pos[d]].child;

		dn = d;
		for (;;) {
			r = ntfs_ixs_node(s, child, d + 1, &leaf);
			if (r != ST_OK)
				goto out_reins;
			d++;
			path[d] = leaf;
			pos[d] = leaf->n;
			if (!leaf->is_node)
				break;
			if (d + 1 >= NTFS_IXS_PATH - 1) {
				r = ntfs_fs_error(s->fs, s->ix.mft_no,
						     "index too deep");
				goto out_reins;
			}
			child = leaf->end_child;
		}
		if (leaf->n == 0) {
			/* No predecessor: only empty nodes below (an index
			 * this driver did not write).  Refuse. */
			r = ntfs_fs_error(s->fs, s->ix.mft_no,
					  "empty index subtree");
			goto out_reins;
		}
		{
			ntfs_ient_t t = ntfs_xnode_take(leaf, leaf->n - 1);

			pred = t.e;
		}
		ntfs_free(nd->ents[pos[dn]].e);
		nd->ents[pos[dn]].e = pred;
		nd->dirty = 1;
		if (dn == 0)
			s->root_dirty = 1;
		/* The node may overflow (a longer key): repair it first. */
		r = ntfs_ixs_fixup(s, path, pos, dn);
		if (r != ST_OK)
			goto out_reins;
		if (leaf->n == 0) {
			int f2;

			/* The path to the empty leaf again: search the
			 * predecessor, then its left subtree's rightmost. */
			ntfs_ixs_key(s, pred, &pk);
			r = ntfs_ixs_search(s, &pk, path, pos, &d, &f2);
			if (r != ST_OK)
				goto out_reins;
			if (!f2 || !path[d]->is_node) {
				r = ntfs_fs_error(s->fs, s->ix.mft_no,
						     "index changed under a "
						     "deletion");
				goto out_reins;
			}
			child = path[d]->ents[pos[d]].child;
			for (;;) {
				ntfs_xnode_t *x;

				r = ntfs_ixs_node(s, child, d + 1, &x);
				if (r != ST_OK)
					goto out_reins;
				d++;
				path[d] = x;
				pos[d] = x->n;
				if (!x->is_node)
					break;
				child = x->end_child;
			}
			if (path[d] != leaf) {
				r = ntfs_fs_error(s->fs, s->ix.mft_no,
						     "index changed under a "
						     "deletion");
				goto out_reins;
			}
			r = ntfs_ixs_empty_leaf(s, path, pos, d, reins, &nre);
		}
	}
	/* Separators of freed leaves go in again from the root -- in a
	 * session of their own after this one is committed
	 * (ntfs_ixs_delete_commit()): written in one commit with the removal,
	 * the leaf that gains a separator could reach the device before the
	 * node that loses it, and a power cut would leave the key twice. */
	if (r == ST_OK && nre) {
		uint8_t **na = (uint8_t **)kalloc((s->nre + nre) *
						  sizeof(uint8_t *));

		if (!na) {
			r = ST_NOMEM;
		} else {
			for (uint32_t i = 0; i < s->nre; i++)
				na[i] = s->reins[i];
			for (uint32_t i = 0; i < nre; i++)
				na[s->nre + i] = reins[i];
			ntfs_free(s->reins);
			s->reins = na;
			s->nre += nre;
			nre = 0;
		}
	}
	for (uint32_t i = 0; i < nre; i++)
		ntfs_free(reins[i]);
	nre = 0;
out_reins:
	for (uint32_t i = 0; i < nre; i++)
		ntfs_free(reins[i]);
	kfree(reins);
	return r;
}

/* Replace the key of the entry with key `k' by `e' (same length: a
 * $FILE_NAME duplicate's sizes / times / attributes).  Unchanged keys are
 * not written. */
int ntfs_ixs_update(ntfs_ixs_t *s, const ntfs_ikey_t *k,
		    const uint8_t *e)
{
	ntfs_xnode_t *path[NTFS_IXS_PATH];
	uint32_t pos[NTFS_IXS_PATH], d;
	ntfs_ient_t *t;
	int found, r;

	r = ntfs_ixs_search(s, k, path, pos, &d, &found);
	if (r != ST_OK)
		return r;
	if (!found)
		return ST_NOT_FOUND;
	t = &path[d]->ents[pos[d]];
	if (((const ntfs_index_entry_t *)t->e)->length !=
	    ((const ntfs_index_entry_t *)e)->length)
		return ST_INVALID;
	if (kmemcmp(t->e, e, ((const ntfs_index_entry_t *)e)->length) == 0)
		return ST_OK;
	mm_memcpy(t->e, e, ((const ntfs_index_entry_t *)e)->length);
	path[d]->dirty = 1;
	if (d == 0)
		s->root_dirty = 1;
	return ST_OK;
}

/* Is the index empty (no entry at all)? */
int ntfs_ixs_empty(ntfs_ixs_t *s, int *empty)
{
	ntfs_xnode_t *nd = s->root;
	uint32_t d = 0;

	for (;;) {
		int r;

		if (nd->n) {
			*empty = 0;
			return ST_OK;
		}
		if (!nd->is_node || nd->end_child < 0) {
			*empty = 1;
			return ST_OK;
		}
		if (++d > NTFS_INDEX_MAX_DEPTH)
			return ST_IO;
		r = ntfs_ixs_node(s, nd->end_child, d, &nd);
		if (r != ST_OK)
			return r;
	}
}

/* Write one block of the session. */
static int ntfs_ixs_write_block(ntfs_ixs_t *s, ntfs_xnode_t *nd)
{
	ntfs_fs_t *fs = s->fs;
	uint32_t ib = s->ix.ib_size, eo = ntfs_ib_eo(ib);
	uint8_t *buf = (uint8_t *)ntfs_zalloc(ib), *prot;
	ntfs_index_block_t *hb = (ntfs_index_block_t *)buf;
	uint32_t used;
	int r;

	if (!buf)
		return ST_NOMEM;
	if (ntfs_xnode_bytes(nd) > ntfs_ib_cap(ib)) {
		kfree(buf);
		WARN(1, "ntfs: index block overflow at write");
		return ST_IO;
	}
	hb->magic = NTFS_MAGIC_INDX;
	hb->usa_ofs = NTFS_INDX_HDR + NTFS_INDEX_HDR;
	hb->usa_count = (uint16_t)(ib / NTFS_BLOCK_SIZE + 1);
	hb->lsn = nd->lsn;
	hb->index_block_vcn = nd->vcn;
	hb->index.entries_offset = eo;
	used = ntfs_xnode_encode(nd, buf + NTFS_INDX_HDR + eo);
	hb->index.index_length = eo + used;
	hb->index.allocated_size = ib - NTFS_INDX_HDR;
	hb->index.flags = nd->is_node ? NTFS_INDEX_NODE : 0;
	ntfs_put_le16(buf + hb->usa_ofs, nd->usn ? nd->usn : 1);
	prot = (uint8_t *)kalloc(ib);
	if (!prot) {
		kfree(buf);
		return ST_NOMEM;
	}
	mm_memcpy(prot, buf, ib);
	r = ntfs_mst_protect(prot, ib);
	if (r == ST_OK) {
		nd->usn = ntfs_le16(prot + hb->usa_ofs);
		ntfs_put_le16(buf + hb->usa_ofs, nd->usn);
		r = ntfs_wt_write(fs, s->alloc,
				  (uint64_t)nd->vcn << s->ix.vcn_bits, prot, ib);
	}
	if (r == ST_OK)
		ntfs_ic_store(fs, s->alloc->handle, nd->vcn, buf, ib);
	else
		ntfs_ic_forget(fs, s->alloc->handle, nd->vcn);
	kfree(prot);
	kfree(buf);
	return r;
}

/* The record part of the commit: root, allocation, bitmap, the owner's
 * times. */
static int ntfs_ixs_commit_record(ntfs_ixs_t *s, int drop_alloc)
{
	ntfs_fs_t *fs = s->fs;
	ntfs_eino_t *e = NULL;
	uint8_t *root = NULL;
	uint32_t used, rlen;
	int r;

	r = ntfs_eino_load(fs, s->mref, &e);
	if (r != ST_OK)
		return r;
	if (s->root_dirty || drop_alloc) {
		ntfs_index_root_t *ir;

		rlen = NTFS_INDEX_ROOT_HDR + NTFS_INDEX_HDR +
		       ntfs_xnode_bytes(s->root);
		root = (uint8_t *)ntfs_zalloc(rlen);
		if (!root) {
			r = ST_NOMEM;
			goto out;
		}
		mm_memcpy(root, s->ir_hdr, NTFS_INDEX_ROOT_HDR);
		ir = (ntfs_index_root_t *)root;
		used = ntfs_xnode_encode(s->root, root + NTFS_INDEX_ROOT_HDR +
							  NTFS_INDEX_HDR);
		ir->index.entries_offset = NTFS_INDEX_HDR;
		ir->index.index_length = NTFS_INDEX_HDR + used;
		ir->index.allocated_size = NTFS_INDEX_HDR + used;
		ir->index.flags = s->root->is_node ? NTFS_LARGE_INDEX :
						     NTFS_SMALL_INDEX;
		r = ntfs_eino_set_res(e, NTFS_AT_INDEX_ROOT, s->name, s->nlen,
				      0, root, rlen);
		if (r != ST_OK)
			goto out;
	}
	if (drop_alloc) {
		r = ntfs_eino_drop(e, NTFS_AT_INDEX_ALLOCATION, s->name,
				   s->nlen, 1);
		if (r == ST_OK)
			r = ntfs_eino_drop(e, NTFS_AT_BITMAP, s->name, s->nlen,
					   1);
		if (r != ST_OK)
			goto out;
	} else {
		if (s->alloc_changed) {
			ntfs_stream_t *a = s->alloc;

			r = ntfs_eino_set_nonres(e, NTFS_AT_INDEX_ALLOCATION,
						 s->name, s->nlen, 0, 0, &a->rl,
						 a->alloc_size, a->data_size,
						 a->init_size, 0);
			if (r != ST_OK)
				goto out;
		}
		if (s->bmp_dirty) {
			r = ntfs_eino_set_value(e, NTFS_AT_BITMAP, s->name,
						s->nlen, s->bmp, s->bmp_len,
						fs->rec_size / 4);
			if (r != ST_OK)
				goto out;
		}
	}
	if (s->touch) {
		long i = ntfs_eino_find(e, NTFS_AT_STANDARD_INFORMATION, NULL,
					0, 0);

		if (i >= 0 && !ntfs_ea_hdr(&e->a[i])->non_resident) {
			ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);
			ntfs_standard_information_t *si =
				(ntfs_standard_information_t
					 *)(e->a[i].b +
					    a->data.resident.value_offset);
			int64_t now = ntfs_now();

			si->last_data_change_time = now;
			si->last_mft_change_time = now;
			e->a[i].dirty = 1;
		}
	}
	r = ntfs_eino_commit(e);
out:
	ntfs_eino_free(e);
	ntfs_free(root);
	return r;
}

/* Commit the session (see the section comment for the order).  The
 * session is spent afterwards. */
int ntfs_ixs_commit(ntfs_ixs_t *s)
{
	ntfs_xnode_t *nd;
	int r = ST_OK, drop_alloc = 0;
	uint32_t maxdepth = 0;

	/* A root with a single child takes the child back when it fits
	 * (the tree loses a level). */
	while (s->root->is_node && s->root->n == 0 && s->root->end_child >= 0) {
		ntfs_xnode_t *c;
		uint32_t need;

		r = ntfs_ixs_node(s, s->root->end_child, 1, &c);
		if (r != ST_OK)
			goto fail;
		need = NTFS_INDEX_ROOT_HDR + NTFS_INDEX_HDR + ntfs_xnode_bytes(c);
		if (need > s->root_cap)
			break;
		ntfs_free(s->root->ents);
		s->root->ents = c->ents;
		s->root->n = c->n;
		s->root->cap = c->cap;
		s->root->is_node = c->is_node;
		s->root->end_child = c->end_child;
		c->ents = NULL;
		c->n = c->cap = 0;
		r = ntfs_ixs_free_block(s, c);
		if (r != ST_OK)
			goto fail;
		s->root_dirty = 1;
	}
	/* Every block free and the root a leaf: no allocation any more. */
	if (!s->root->is_node && s->alloc &&
	    !(s->alloc->flags & NTFS_ST_NOALLOC)) {
		int any = 0;

		r = ntfs_ixs_bmp_load(s);
		if (r != ST_OK)
			goto fail;
		for (uint32_t i = 0; i < s->bmp_len && !any; i++)
			any = s->bmp[i] != 0;
		drop_alloc = !any;
	}
	/* 1. New blocks: nothing refers to them yet, so a failure here is
	 * undone by ntfs_ixs_abort(). */
	for (nd = s->nodes; nd; nd = nd->next) {
		if (nd->depth > maxdepth)
			maxdepth = nd->depth;
		if (nd->is_new && !nd->freed) {
			r = ntfs_ixs_write_block(s, nd);
			if (r != ST_OK)
				goto fail;
		}
	}
	r = ntfs_barrier(s->fs);
	if (r != ST_OK)
		goto fail;
	/* 2. New blocks (a free bit taken, or a grown allocation): the
	 * record describes the new clusters and the bitmap marks the new
	 * blocks in use before any old block or the root can refer to them.
	 * The root keeps its old contents for now, and bits this session
	 * cleared stay set until the last step (the blocks they describe
	 * are still referenced on the device).  If this commit does not
	 * reach the device nothing changed (abort). */
	{
		int newblk = s->alloc_changed;

		for (nd = s->nodes; nd && !newblk; nd = nd->next)
			if (nd->is_new && !nd->freed)
				newblk = 1;
		if (newblk && !drop_alloc) {
			int rd = s->root_dirty, tch = s->touch, bd = s->bmp_dirty;
			uint8_t *fin = s->bmp, *mid = NULL;

			if (s->bmp_len) {
				mid = (uint8_t *)kalloc(s->bmp_len);
				if (!mid) {
					r = ST_NOMEM;
					goto fail;
				}
				mm_memcpy(mid, fin, s->bmp_len);
				for (uint32_t i = 0; i < s->bmp_len &&
						     i < s->bmp0_len; i++)
					mid[i] |= s->bmp0[i];
				s->bmp = mid;
			}
			s->root_dirty = 0;
			s->touch = 0;
			r = ntfs_ixs_commit_record(s, 0);
			s->root_dirty = rd;
			s->touch = tch;
			s->bmp = fin;
			if (r == ST_OK)
				bd = mid && kmemcmp(mid, fin, s->bmp_len) != 0;
			ntfs_free(mid);
			if (r != ST_OK)
				goto fail;
			s->nnew = 0; /* referenced now */
			s->alloc_changed = 0;
			s->bmp_dirty = bd;
			r = ntfs_barrier(s->fs);
			if (r != ST_OK)
				goto broken;
		}
	}
	/* From here on the update is committed: a failure leaves the index
	 * inconsistent and is never undone (callers would remove records
	 * an entry may already name); the mount is latched read-only and
	 * the volume stays dirty for a check.
	 *
	 * 3. Changed old blocks, the deepest first, with a device flush
	 * after each level: a block that loses entries (a split leaf, the
	 * leaf whose predecessor moves up) reaches the device before the
	 * block that gains them or the pointer to them, so a power cut
	 * leaves at worst an entry that is missing (an orphan a check
	 * recovers), never one that is there twice. */
	for (uint32_t depth = maxdepth; depth >= 1; depth--) {
		int wrote = 0;

		for (nd = s->nodes; nd; nd = nd->next) {
			if (nd->depth != depth || nd->is_new || nd->freed ||
			    !nd->dirty || nd->vcn < 0)
				continue;
			r = ntfs_ixs_write_block(s, nd);
			if (r != ST_OK)
				goto broken;
			wrote = 1;
		}
		if (wrote && ntfs_barrier(s->fs) != ST_OK)
			goto broken;
	}
	/* 4. The record last: the root (it gains references last), freed
	 * blocks in the bitmap, a dropped allocation, the owner's times. */
	if (s->root_dirty || s->bmp_dirty || s->alloc_changed || s->touch ||
	    drop_alloc) {
		r = ntfs_ixs_commit_record(s, drop_alloc);
		if (r != ST_OK)
			goto broken;
		s->nnew = 0;
	}
	if (drop_alloc && s->alloc) {
		ntfs_stream_t *a = s->alloc;

		pagecache_invalidate_file(&s->fs->sb, ntfs_chain(a));
		ntfs_ic_drop_stream(s->fs, a->handle);
		ntfs_rl_free(&a->rl);
		a->flags |= NTFS_ST_NOALLOC;
		a->data_size = a->init_size = a->alloc_size = 0;
	}
	s->alloc_changed = 0;
	ntfs_ixs_free(s);
	return ST_OK;
broken:
	ntfs_fs_broken(s->fs, s->ix.mft_no, "index update interrupted");
	s->alloc_changed = 0;
	s->nnew = 0; /* whatever was allocated may be referenced: leaked */
	if (s->alloc)
		ntfs_ic_drop_stream(s->fs, s->alloc->handle);
	ntfs_ixs_free(s);
	return ST_OK;
fail:
	ntfs_ixs_abort(s);
	return r;
}

/* Delete key `k' (see ntfs_ixs_delete()) and commit; the separators of
 * leaves the deletion freed go in again in a second session, after the
 * first one is on the device.  A power cut between the two leaves those
 * keys out of the index (names a check puts back), never twice in it.
 * Once the first commit is done the deletion stands: a failure of the
 * second one latches the mount (the volume stays dirty) and is not
 * reported as a failed deletion.  The session is spent afterwards. */
int ntfs_ixs_delete_commit(ntfs_ixs_t *s, const ntfs_ikey_t *k,
			   uint64_t mref)
{
	ntfs_fs_t *fs = s->fs;
	uint64_t owner = s->mref;
	uint16_t name[8];
	uint32_t nlen = s->nlen, n;
	uint8_t **re;
	int r;

	r = ntfs_ixs_delete(s, k, mref);
	if (r != ST_OK) {
		ntfs_ixs_abort(s);
		return r;
	}
	mm_memcpy(name, s->name, sizeof(name));
	re = s->reins;
	n = s->nre;
	s->reins = NULL;
	s->nre = 0;
	r = ntfs_ixs_commit(s);
	if (r == ST_OK && n && !fs->latched_ro) {
		ntfs_ixs_t *s2 = NULL;
		int r2 = ntfs_ixs_open(fs, owner, name, nlen, &s2);

		for (uint32_t i = 0; i < n; i++) {
			if (r2 == ST_OK)
				r2 = ntfs_ixs_insert(s2, re[i]); /* takes it */
			else
				ntfs_free(re[i]);
			re[i] = NULL;
		}
		if (r2 == ST_OK)
			r2 = ntfs_ixs_commit(s2);
		else if (s2)
			ntfs_ixs_abort(s2);
		if (r2 != ST_OK)
			ntfs_fs_broken(fs, NTFS_MREF_NO(owner),
				       "index entries could not be put back");
		n = 0;
	}
	for (uint32_t i = 0; i < n; i++)
		ntfs_free(re[i]);
	ntfs_free(re);
	return r;
}

/* ---- $FILE_NAME keys ---- */

/* Insert the $I30 entry for (fn, mref) into directory `dir'; `touch'
 * updates the directory's times in the same record write. */
int ntfs_dir_add_entry(ntfs_fs_t *fs, uint64_t dir, uint64_t mref,
		       const uint8_t *fn, uint32_t fl, int touch)
{
	ntfs_ixs_t *s;
	uint8_t *ie;
	int r;

	ie = ntfs_ie_build(mref, fn, fl);
	if (!ie)
		return ST_NOMEM;
	r = ntfs_ixs_open(fs, dir, ntfs_i30_name, 4, &s);
	if (r != ST_OK) {
		kfree(ie);
		return r;
	}
	s->touch = touch;
	r = ntfs_ixs_insert(s, ie);
	if (r != ST_OK) {
		ntfs_ixs_abort(s);
		return r;
	}
	return ntfs_ixs_commit(s);
}

/* Remove the $I30 entry `name' of file `mref' from directory `dir'. */
int ntfs_dir_del_entry(ntfs_fs_t *fs, uint64_t dir, uint64_t mref,
		       const uint16_t *name, uint32_t nlen, int touch)
{
	ntfs_ixs_t *s;
	ntfs_ikey_t k;
	int r;

	r = ntfs_ixs_open(fs, dir, ntfs_i30_name, 4, &s);
	if (r != ST_OK)
		return r;
	s->touch = touch;
	k.key = name;
	k.len = nlen;
	k.ic = 0;
	r = ntfs_ixs_delete_commit(s, &k, mref);
	if (r != ST_OK)
		return r;
	/* A duplicate of the entry (left by an interrupted update of this
	 * or another system) goes too: no entry may survive that names a
	 * file which is being removed from this directory. */
	for (int dup = 0; r == ST_OK && dup < 8; dup++) {
		int found = 0;

		if (ntfs_ixs_open(fs, dir, ntfs_i30_name, 4, &s) != ST_OK)
			break;
		{
			ntfs_xnode_t *path[NTFS_IXS_PATH];
			uint32_t pos[NTFS_IXS_PATH], d;

			if (ntfs_ixs_search(s, &k, path, pos, &d, &found) !=
			    ST_OK)
				found = 0;
		}
		if (!found) {
			ntfs_ixs_abort(s);
			break;
		}
		/* Repaired, but the volume keeps its dirty flag. */
		WARN_RATELIMIT(1, "ntfs: directory %llu: duplicate entry "
				  "removed",
			       (unsigned long long)NTFS_MREF_NO(dir));
		fs->errors = 1;
		if (ntfs_ixs_delete_commit(s, &k, mref) != ST_OK)
			break;
	}
	return r;
}
