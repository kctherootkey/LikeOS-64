/* LikeOS - NTFS filesystem driver
 *
 * Attributes: validation, search contexts, editable inodes.
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

/* Is the type resident-only? */
static int ntfs_type_resident_only(uint32_t type)
{
	switch (type) {
	case NTFS_AT_STANDARD_INFORMATION:
	case NTFS_AT_FILE_NAME:
	case NTFS_AT_OBJECT_ID:
	case NTFS_AT_VOLUME_NAME:
	case NTFS_AT_VOLUME_INFORMATION:
	case NTFS_AT_INDEX_ROOT:
	case NTFS_AT_EA_INFORMATION:
		return 1;
	default:
		return 0;
	}
}

/* Validate the attribute at `aoff' of record `rec' (record number `no' for
 * the messages).  ST_OK, or ST_IO. */
int ntfs_attr_valid(ntfs_fs_t *fs, const uint8_t *rec, uint32_t aoff,
		    uint64_t no)
{
	const ntfs_mft_record_t *m = (const ntfs_mft_record_t *)rec;
	const ntfs_attr_record_t *a;
	uint32_t in_use = m->bytes_in_use;
	uint32_t len, nlen2, noff;
	const char *why = NULL;

	if ((uint64_t)aoff + 16 > in_use) {
		why = "header outside the record";
		goto bad;
	}
	a = (const ntfs_attr_record_t *)(rec + aoff);
	len = a->length;
	if (len < 16 || (len & 7) || len > in_use - aoff) {
		why = "bad length";
		goto bad;
	}
	if (a->non_resident > 1) {
		why = "bad residency";
		goto bad;
	}
	nlen2 = (uint32_t)a->name_length * 2U;
	noff = a->name_offset;
	if (a->name_length && (noff > len || nlen2 > len - noff)) {
		why = "name outside the attribute";
		goto bad;
	}
	if (!a->non_resident) {
		uint32_t vo = a->data.resident.value_offset;
		uint32_t vl = a->data.resident.value_length;
		uint32_t min = 0;

		if (len < NTFS_ATTR_RESIDENT_HDR) {
			why = "short resident header";
			goto bad;
		}
		if (vo < NTFS_ATTR_RESIDENT_HDR || vl > len || vo > len - vl) {
			why = "value outside the attribute";
			goto bad;
		}
		if (a->name_length &&
		    (noff < NTFS_ATTR_RESIDENT_HDR ||
		     (vl && noff < vo + vl && noff + nlen2 > vo))) {
			why = "name overlaps the header or the value";
			goto bad;
		}
		switch (a->type) {
		case NTFS_AT_STANDARD_INFORMATION:
			min = NTFS_SI_SIZE_V12;
			break;
		case NTFS_AT_FILE_NAME:
			min = NTFS_FN_HDR + 2;
			break;
		case NTFS_AT_VOLUME_INFORMATION:
			min = 12;
			break;
		case NTFS_AT_EA_INFORMATION:
			min = 8;
			break;
		case NTFS_AT_INDEX_ROOT:
			min = NTFS_INDEX_ROOT_HDR + NTFS_INDEX_HDR + NTFS_IE_HDR;
			break;
		default:
			break;
		}
		if (vl < min) {
			why = "value shorter than its type allows";
			goto bad;
		}
		if (a->type == NTFS_AT_FILE_NAME) {
			const ntfs_file_name_t *fn =
				(const ntfs_file_name_t *)(rec + aoff + vo);

			if (fn->file_name_length == 0 ||
			    NTFS_FN_HDR + 2U * fn->file_name_length > vl ||
			    fn->file_name_type > NTFS_FILE_NAME_WIN32_AND_DOS) {
				why = "bad $FILE_NAME";
				goto bad;
			}
		}
		if (a->type == NTFS_AT_VOLUME_NAME &&
		    ((vl & 1) || vl > 2U * NTFS_MAX_LABEL_LEN)) {
			why = "bad $VOLUME_NAME";
			goto bad;
		}
	} else {
		int64_t lowest = a->data.non_resident.lowest_vcn;
		int64_t highest = a->data.non_resident.highest_vcn;
		uint32_t hdr = NTFS_ATTR_NONRES_HDR;
		uint32_t mpo = a->data.non_resident.mapping_pairs_offset;

		if (ntfs_type_resident_only(a->type)) {
			why = "non-resident record of a resident-only type";
			goto bad;
		}
		if ((a->flags & (NTFS_ATTR_COMPRESSION_MASK |
				 NTFS_ATTR_IS_SPARSE)) &&
		    lowest == 0)
			hdr = NTFS_ATTR_NONRES_HDR_COMP;
		if (len < hdr) {
			why = "short non-resident header";
			goto bad;
		}
		if (mpo < NTFS_ATTR_NONRES_HDR || mpo < hdr || mpo > len) {
			why = "mapping pairs outside the attribute";
			goto bad;
		}
		if (a->name_length && (noff < hdr || noff + nlen2 > mpo)) {
			why = "name overlaps the header or the mapping pairs";
			goto bad;
		}
		if (lowest < 0 || highest < lowest - 1) {
			why = "bad VCN range";
			goto bad;
		}
		if (lowest == 0) {
			int64_t as = a->data.non_resident.allocated_size;
			int64_t ds = a->data.non_resident.data_size;
			int64_t is = a->data.non_resident.initialized_size;

			if (as < 0 || ds < 0 || is < 0 || is > ds || ds > as ||
			    ((uint64_t)as & (fs->cluster_size - 1))) {
				why = "sizes exceed the allocation";
				goto bad;
			}
		}
	}
	return ST_OK;
bad:
	WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt attribute at 0x%x: %s",
		       (unsigned long long)no, aoff, why);
	return ST_IO;
}

/* Validate a whole attribute list. */
int ntfs_al_valid(ntfs_fs_t *fs, const uint8_t *al, uint32_t len,
		  uint64_t no)
{
	uint32_t pos = 0;

	while (pos < len) {
		const ntfs_attr_list_entry_t *e;
		uint32_t elen;

		if (len - pos < NTFS_ALE_HDR)
			goto bad;
		e = (const ntfs_attr_list_entry_t *)(al + pos);
		elen = e->length;
		if (elen < NTFS_ALE_HDR || (elen & 7) || elen > len - pos)
			goto bad;
		if (e->name_length &&
		    (e->name_offset < NTFS_ALE_HDR ||
		     (uint32_t)e->name_offset + 2U * e->name_length > elen))
			goto bad;
		if (NTFS_MREF_NO(e->mft_reference) >= fs->mft_records &&
		    fs->mft_records)
			goto bad;
		if (e->type == NTFS_AT_UNUSED || e->type == NTFS_AT_END ||
		    e->lowest_vcn < 0)
			goto bad;
		pos += elen;
	}
	return ST_OK;
bad:
	WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt attribute list "
			  "entry at 0x%x",
		       (unsigned long long)no, pos);
	return ST_IO;
}

/* Does (name, nlen) match the filter? */
int ntfs_name_match(ntfs_fs_t *fs, const uint16_t *name, uint32_t nlen,
		    const uint16_t *want, uint32_t wlen, int flags)
{
	if (!want)
		return 1; /* any name */
	if (nlen != wlen)
		return 0;
	if (!nlen)
		return 1;
	if (flags & NTFS_FIND_IC)
		return ntfs_upcase_eq(fs, name, want, nlen);
	return kmemcmp(name, want, nlen * 2U) == 0;
}

/* Locate attribute (type, instance) in record `rec' (record `no'). */
ntfs_attr_record_t *ntfs_rec_attr(ntfs_fs_t *fs, uint8_t *rec,
				  uint64_t no, uint32_t type,
				  int by_instance, uint16_t instance,
				  int unnamed)
{
	ntfs_mft_record_t *m = (ntfs_mft_record_t *)rec;
	uint32_t off = m->attrs_offset;

	while (off + 4 <= m->bytes_in_use) {
		ntfs_attr_record_t *a = (ntfs_attr_record_t *)(rec + off);

		if (a->type == NTFS_AT_END)
			return NULL;
		if (ntfs_attr_valid(fs, rec, off, no) != ST_OK)
			return NULL;
		if (a->type == type && (!by_instance || a->instance == instance) &&
		    (!unnamed || a->name_length == 0))
			return a;
		off += a->length;
	}
	return NULL;
}

/* ===========================================================================
 * S8  Attributes
 *
 * ntfs_attr_valid() is THE attribute validator.  Every attribute record is
 * checked by it before any of its fields beyond type/length is used, by
 * every walker (base record, extension records reached through an
 * attribute list, enumeration of all attributes):
 *
 *   - type, length: length >= 16, 8-aligned, inside bytes_in_use;
 *   - name: name_offset + 2 * name_length inside the record;
 *   - resident: value_offset >= the 24-byte header, value inside
 *     the record, the name not overlapping the value, a per-type
 *     minimum value size matching $AttrDef: $STANDARD_INFORMATION
 *     48, $FILE_NAME 66 + 2 * name length >= 68, $VOLUME_INFORMATION
 *     12, $EA_INFORMATION 8, $INDEX_ROOT 48; $VOLUME_NAME even and
 *     at most 256 bytes;
 *   - non-resident: never for a resident-only type; header of 64
 *     bytes, 72 for the first extent of a compressed / sparse
 *     attribute; mapping_pairs_offset behind the header and inside the
 *     attribute; the name between the header and the mapping pairs;
 *     lowest_vcn >= 0, highest_vcn >= lowest_vcn - 1; on the first
 *     extent 0 <= initialized <= data <= allocated, allocated a multiple
 *     of the cluster size.
 *
 * The search context (ntfs_actx_t) walks one file's attributes: the base
 * record alone, or -- when the file has an $ATTRIBUTE_LIST -- the list's
 * entries, loading the extension record each one points to.  The list is
 * validated as a whole when it is loaded: fixed header
 * inside the list before any field is read, length a multiple of 8
 * covering header and name, a live MFT reference, entries tiling the list
 * exactly, at most 256 KB.  An extension record must be in use and name
 * the base record as its base, with the sequence number the entry gives.
 *
 * Results point into the context's record buffers and stay valid until the
 * next find / reset / close on the context.
 * =========================================================================== */

/* The filter "unnamed attribute" (a non-NULL name of length 0). */
const uint16_t ntfs_unnamed[1] = { 0 };

static ntfs_actx_t *ntfs_actx_alloc(ntfs_fs_t *fs)
{
	ntfs_actx_t *c = (ntfs_actx_t *)ntfs_zalloc(sizeof(*c));

	if (!c)
		return NULL;
	c->fs = fs;
	c->base = (uint8_t *)kalloc(fs->rec_size);
	c->ext = (uint8_t *)kalloc(fs->rec_size);
	c->ext_no = ~0ULL;
	if (!c->base || !c->ext) {
		ntfs_free(c->base);
		ntfs_free(c->ext);
		kfree(c);
		return NULL;
	}
	return c;
}

void ntfs_actx_close(ntfs_actx_t *c)
{
	if (!c)
		return;
	ntfs_free(c->base);
	ntfs_free(c->ext);
	ntfs_free(c->al);
	kfree(c);
}

/* Start the walk over again. */
void ntfs_actx_reset(ntfs_actx_t *c)
{
	c->pos = c->al ? 0 : ((ntfs_mft_record_t *)c->base)->attrs_offset;
	c->rec = NULL;
	c->a = NULL;
	c->ale = NULL;
}

static int ntfs_attr_readall_ctx(ntfs_actx_t *c, uint32_t cap, uint8_t **out,
				 uint32_t *out_len);

/* Load the base record's attribute list into the context, if it has one. */
static int ntfs_actx_load_al(ntfs_actx_t *c)
{
	ntfs_fs_t *fs = c->fs;
	ntfs_mft_record_t *m = (ntfs_mft_record_t *)c->base;
	uint32_t off = m->attrs_offset;

	while (off + 4 <= m->bytes_in_use) {
		ntfs_attr_record_t *a = (ntfs_attr_record_t *)(c->base + off);
		uint8_t *buf = NULL;
		uint32_t blen = 0;
		int r;

		if (a->type == NTFS_AT_END)
			break;
		r = ntfs_attr_valid(fs, c->base, off, c->base_no);
		if (r != ST_OK)
			return r;
		if (a->type > NTFS_AT_ATTRIBUTE_LIST)
			break; /* attributes are sorted by type */
		if (a->type != NTFS_AT_ATTRIBUTE_LIST) {
			off += a->length;
			continue;
		}
		/* The size is checked as 64 bits before allocating. */
		if (a->non_resident &&
		    (uint64_t)a->data.non_resident.data_size >
			    NTFS_ATTR_LIST_MAX) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: attribute "
					  "list too large",
				       (unsigned long long)c->base_no);
			return ST_IO;
		}
		c->rec = c->base;
		c->rec_no = c->base_no;
		c->a = a;
		c->aoff = off;
		r = ntfs_attr_readall_ctx(c, NTFS_ATTR_LIST_MAX, &buf, &blen);
		if (r != ST_OK)
			return r;
		r = ntfs_al_valid(fs, buf, blen, c->base_no);
		if (r != ST_OK) {
			ntfs_free(buf);
			return r;
		}
		c->al = buf;
		c->al_len = blen;
		return ST_OK;
	}
	return ST_OK;
}

/* Open a search context on the file `mref' (record number, and sequence
 * number unless it is 0).  ST_NOT_FOUND: the record is not in use;
 * ST_IO: corrupt, a stale reference or an extension record. */
int ntfs_actx_open(ntfs_fs_t *fs, uint64_t mref, ntfs_actx_t **out)
{
	ntfs_actx_t *c;
	ntfs_mft_record_t *m;
	uint64_t no = NTFS_MREF_NO(mref);
	uint16_t seq = NTFS_MREF_SEQ(mref);
	int r;

	*out = NULL;
	c = ntfs_actx_alloc(fs);
	if (!c)
		return ST_NOMEM;
	r = ntfs_mft_read(fs, no, c->base);
	if (r != ST_OK)
		goto fail;
	m = (ntfs_mft_record_t *)c->base;
	if (!(m->flags & NTFS_MFT_REC_IN_USE)) {
		r = ST_NOT_FOUND;
		goto fail;
	}
	if (m->base_mft_record != 0) {
		/* An extension record is not a file. */
		WARN_RATELIMIT(1, "ntfs: reference to extension record %llu",
			       (unsigned long long)no);
		r = ST_IO;
		goto fail;
	}
	if (seq && m->sequence_number && seq != m->sequence_number) {
		/* A stale reference is corrupt metadata. */
		WARN_RATELIMIT(1, "ntfs: stale reference to MFT record %llu "
				  "(sequence %u, record has %u)",
			       (unsigned long long)no, seq,
			       m->sequence_number);
		r = ST_IO;
		goto fail;
	}
	c->base_no = no;
	c->base_seq = m->sequence_number;
	r = ntfs_actx_load_al(c);
	if (r != ST_OK)
		goto fail;
	ntfs_actx_reset(c);
	*out = c;
	return ST_OK;
fail:
	ntfs_actx_close(c);
	return r;
}

/* Find, in record `rec', the attribute named by list entry `e'. */
static int ntfs_actx_locate(ntfs_actx_t *c, uint8_t *rec, uint64_t rec_no,
			    const ntfs_attr_list_entry_t *e)
{
	ntfs_mft_record_t *m = (ntfs_mft_record_t *)rec;
	uint32_t off = m->attrs_offset;
	const uint16_t *en = (const uint16_t *)((const uint8_t *)e +
						e->name_offset);

	while (off + 4 <= m->bytes_in_use) {
		ntfs_attr_record_t *a = (ntfs_attr_record_t *)(rec + off);

		if (a->type == NTFS_AT_END)
			break;
		if (ntfs_attr_valid(c->fs, rec, off, rec_no) != ST_OK)
			return ST_IO;
		if (a->type == e->type && a->instance == e->instance) {
			if (a->name_length != e->name_length ||
			    (e->name_length &&
			     kmemcmp(ntfs_attr_name(a), en,
				     2U * e->name_length) != 0) ||
			    (a->non_resident &&
			     a->data.non_resident.lowest_vcn != e->lowest_vcn))
				break;
			c->rec = rec;
			c->rec_no = rec_no;
			c->a = a;
			c->aoff = off;
			return ST_OK;
		}
		off += a->length;
	}
	WARN_RATELIMIT(1, "ntfs: MFT record %llu: attribute list entry "
			  "(type 0x%x instance %u) has no attribute",
		       (unsigned long long)c->base_no, e->type, e->instance);
	return ST_IO;
}

/* Find the next attribute of `type' (NTFS_AT_UNUSED: any) whose name
 * matches (`name' NULL: any; else exactly `nlen' units, 0 = unnamed).
 * ST_OK (c->a, c->rec), ST_NOT_FOUND at the end, ST_IO when corrupt. */
int ntfs_actx_find(ntfs_actx_t *c, uint32_t type, const uint16_t *name,
		   uint32_t nlen, int flags)
{
	ntfs_fs_t *fs = c->fs;

	c->a = NULL;
	c->ale = NULL;
	if (!c->al) {
		ntfs_mft_record_t *m = (ntfs_mft_record_t *)c->base;

		while (c->pos + 4 <= m->bytes_in_use) {
			uint32_t off = c->pos;
			ntfs_attr_record_t *a =
				(ntfs_attr_record_t *)(c->base + off);

			if (a->type == NTFS_AT_END)
				return ST_NOT_FOUND;
			if (ntfs_attr_valid(fs, c->base, off, c->base_no) !=
			    ST_OK)
				return ST_IO;
			c->pos = off + a->length;
			if (type != NTFS_AT_UNUSED && a->type != type)
				continue;
			if (!ntfs_name_match(fs, ntfs_attr_name(a),
					     a->name_length, name, nlen, flags))
				continue;
			c->rec = c->base;
			c->rec_no = c->base_no;
			c->a = a;
			c->aoff = off;
			return ST_OK;
		}
		/* The record ended without an AT_END marker. */
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: no end marker",
			       (unsigned long long)c->base_no);
		return ST_IO;
	}
	while (c->pos < c->al_len) {
		const ntfs_attr_list_entry_t *e =
			(const ntfs_attr_list_entry_t *)(c->al + c->pos);
		const uint16_t *en = (const uint16_t *)((const uint8_t *)e +
							e->name_offset);
		uint64_t eno = NTFS_MREF_NO(e->mft_reference);
		uint16_t eseq = NTFS_MREF_SEQ(e->mft_reference);
		uint8_t *rec;
		int r;

		c->pos += e->length;
		if (type != NTFS_AT_UNUSED && e->type != type)
			continue;
		if (!ntfs_name_match(fs, en, e->name_length, name, nlen,
				     flags))
			continue;
		if (eno == c->base_no) {
			rec = c->base;
		} else {
			if (c->ext_no != eno) {
				ntfs_mft_record_t *xm =
					(ntfs_mft_record_t *)c->ext;

				c->ext_no = ~0ULL;
				r = ntfs_mft_read(fs, eno, c->ext);
				if (r != ST_OK)
					return r == ST_NOMEM ? r : ST_IO;
				if (!(xm->flags & NTFS_MFT_REC_IN_USE) ||
				    NTFS_MREF_NO(xm->base_mft_record) !=
					    c->base_no ||
				    (eseq && xm->sequence_number &&
				     eseq != xm->sequence_number)) {
					WARN_RATELIMIT(1, "ntfs: MFT record "
							  "%llu: bad extension "
							  "record %llu",
						       (unsigned long long)
							       c->base_no,
						       (unsigned long long)eno);
					return ST_IO;
				}
				c->ext_no = eno;
			}
			rec = c->ext;
		}
		r = ntfs_actx_locate(c, rec, eno, e);
		if (r != ST_OK)
			return r;
		c->ale = e;
		return ST_OK;
	}
	return ST_NOT_FOUND;
}

/* ---- Whole-attribute reads (uncached) ---------------------------------------
 *
 * Small metadata attributes (attribute lists, EAs, reparse data, $AttrDef,
 * $UpCase, $Volume values, the first page of hiberfil.sys) are read whole
 * into a buffer, straight from the device, without involving the page
 * cache or a stream handle.  The size is capped by the caller;
 * bytes beyond initialized_size read as zeros, sparse runs as zeros. */

/* Build the runlist of attribute (type, name) of file `mref' across all
 * its extents.  `*first' receives a copy of the first extent's sizes. */
int ntfs_attr_runlist(ntfs_fs_t *fs, uint64_t mref, uint32_t type,
		      const uint16_t *name, uint32_t nlen,
		      ntfs_runlist_t *rl, ntfs_attr_sizes_t *sz)
{
	ntfs_actx_t *c;
	uint32_t extents = 0;
	int r, have_first = 0;

	mm_memset(rl, 0, sizeof(*rl));
	r = ntfs_actx_open(fs, mref, &c);
	if (r != ST_OK)
		return r;
	for (;;) {
		ntfs_attr_record_t *a;

		r = ntfs_actx_find(c, type, name ? name : ntfs_unnamed,
				   name ? nlen : 0, 0);
		if (r == ST_NOT_FOUND) {
			r = have_first ? ST_OK : ST_NOT_FOUND;
			break;
		}
		if (r != ST_OK)
			break;
		a = c->a;
		if (!a->non_resident) {
			if (have_first) {
				r = ST_IO;
				break;
			}
			if (sz) {
				mm_memset(sz, 0, sizeof(*sz));
				sz->resident = 1;
				sz->data_size = sz->init_size = sz->alloc_size =
					a->data.resident.value_length;
				sz->flags = a->flags;
			}
			r = ST_OK;
			break;
		}
		if (++extents > NTFS_MAX_EXTENTS) {
			r = ST_IO;
			break;
		}
		if (a->data.non_resident.lowest_vcn == 0) {
			if (have_first) {
				r = ST_IO; /* two first extents */
				break;
			}
			have_first = 1;
			if (sz) {
				mm_memset(sz, 0, sizeof(*sz));
				sz->alloc_size = (uint64_t)
					a->data.non_resident.allocated_size;
				sz->data_size =
					(uint64_t)a->data.non_resident.data_size;
				sz->init_size = (uint64_t)
					a->data.non_resident.initialized_size;
				sz->flags = a->flags;
				sz->comp_unit =
					a->data.non_resident.compression_unit;
				if (a->flags & (NTFS_ATTR_COMPRESSION_MASK |
						NTFS_ATTR_IS_SPARSE))
					sz->comp_size = (uint64_t)a->data
								.non_resident
								.compressed_size;
			}
		} else if (!have_first) {
			r = ST_IO; /* extents out of order */
			break;
		}
		r = ntfs_mp_decode(fs, a, a->length, rl);
		if (r != ST_OK) {
			WARN_RATELIMIT(1, "ntfs: MFT record %llu: corrupt "
					  "mapping pairs (type 0x%x)",
				       (unsigned long long)NTFS_MREF_NO(mref),
				       type);
			break;
		}
	}
	ntfs_actx_close(c);
	if (r != ST_OK)
		ntfs_rl_free(rl);
	return r;
}

/* Read [off, off + len) of a non-resident attribute described by `rl'
 * into `dst', zero beyond `init'.  Whole clusters are read through a
 * bounce buffer where the range does not cover them. */
int ntfs_rl_pread(ntfs_fs_t *fs, const ntfs_runlist_t *rl, uint64_t off,
		  uint64_t len, uint64_t init, uint8_t *dst)
{
	uint8_t *bounce = NULL;
	int r = ST_OK;

	might_sleep();
	while (len) {
		int64_t vcn = (int64_t)(off >> fs->cluster_bits);
		uint32_t in = (uint32_t)(off & (fs->cluster_size - 1));
		uint64_t n = fs->cluster_size - in;
		int64_t lcn, left;

		if (n > len)
			n = len;
		if (off >= init) {
			mm_memset(dst, 0, len);
			break;
		}
		if (n > init - off)
			n = init - off;
		r = ntfs_rl_map(rl, vcn, &lcn, &left);
		if (r != ST_OK)
			break; /* unmappable below the size: EIO */
		if (lcn == NTFS_LCN_HOLE) {
			mm_memset(dst, 0, n);
		} else if (in == 0 && n == fs->cluster_size) {
			/* Whole clusters straight into the destination. */
			uint64_t whole = (len < init - off ? len : init - off) >>
					 fs->cluster_bits;

			if (whole > (uint64_t)left)
				whole = (uint64_t)left;
			if (whole == 0)
				whole = 1;
			r = ntfs_read_clusters(fs, lcn, whole, dst);
			if (r != ST_OK)
				break;
			n = whole << fs->cluster_bits;
		} else {
			if (!bounce) {
				bounce = (uint8_t *)kalloc(fs->cluster_size);
				if (!bounce) {
					r = ST_NOMEM;
					break;
				}
			}
			r = ntfs_read_clusters(fs, lcn, 1, bounce);
			if (r != ST_OK)
				break;
			mm_memcpy(dst, bounce + in, n);
		}
		dst += n;
		off += n;
		len -= n;
	}
	ntfs_free(bounce);
	return r;
}

/* Read the attribute the context points at (its first extent) whole.
 * ST_IO when larger than `cap', compressed or encrypted. */
static int ntfs_attr_readall_ctx(ntfs_actx_t *c, uint32_t cap, uint8_t **out,
				 uint32_t *out_len)
{
	ntfs_fs_t *fs = c->fs;
	ntfs_attr_record_t *a = c->a;
	ntfs_runlist_t rl;
	ntfs_attr_sizes_t sz;
	uint16_t *namebuf;
	uint8_t *buf;
	uint32_t nlen;
	int r;

	*out = NULL;
	*out_len = 0;
	if (!a)
		return ST_INVALID;
	if (!a->non_resident) {
		uint32_t vl = a->data.resident.value_length;

		if (vl > cap)
			return ST_IO;
		buf = (uint8_t *)kalloc(vl ? vl : 1);
		if (!buf)
			return ST_NOMEM;
		mm_memcpy(buf, ntfs_attr_value(a), vl);
		*out = buf;
		*out_len = vl;
		return ST_OK;
	}
	if (a->data.non_resident.lowest_vcn != 0)
		return ST_IO;
	if ((uint64_t)a->data.non_resident.data_size > cap)
		return ST_IO;
	if (a->flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_IS_ENCRYPTED))
		return ST_UNSUPPORTED;
	nlen = a->name_length;
	if (!c->al) {
		/* Without an attribute list a non-resident attribute has
		 * exactly one extent, in the base record (this is also how
		 * the attribute list itself is read). */
		mm_memset(&rl, 0, sizeof(rl));
		mm_memset(&sz, 0, sizeof(sz));
		sz.alloc_size = (uint64_t)a->data.non_resident.allocated_size;
		sz.data_size = (uint64_t)a->data.non_resident.data_size;
		sz.init_size = (uint64_t)a->data.non_resident.initialized_size;
		r = ntfs_mp_decode(fs, a, a->length, &rl);
		if (r != ST_OK) {
			ntfs_rl_free(&rl);
			return ST_IO;
		}
	} else {
		/* The name is copied: the walk below reuses the context's
		 * record buffers. */
		namebuf = (uint16_t *)kalloc(nlen ? nlen * 2U : 2U);
		if (!namebuf)
			return ST_NOMEM;
		if (nlen)
			mm_memcpy(namebuf, ntfs_attr_name(a), nlen * 2U);
		r = ntfs_attr_runlist(fs,
				      NTFS_MAKE_MREF(c->base_no, c->base_seq),
				      a->type, nlen ? namebuf : ntfs_unnamed,
				      nlen, &rl, &sz);
		kfree(namebuf);
		if (r != ST_OK)
			return r == ST_NOT_FOUND ? ST_IO : r;
	}
	/* The runlist must describe every byte of the data. */
	if ((uint64_t)rl.end_vcn << fs->cluster_bits < sz.data_size &&
	    sz.data_size) {
		ntfs_rl_free(&rl);
		return ST_IO;
	}
	buf = (uint8_t *)kalloc(sz.data_size ? sz.data_size : 1);
	if (!buf) {
		ntfs_rl_free(&rl);
		return ST_NOMEM;
	}
	r = ntfs_rl_pread(fs, &rl, 0, sz.data_size, sz.init_size, buf);
	ntfs_rl_free(&rl);
	if (r != ST_OK) {
		kfree(buf);
		return r;
	}
	*out = buf;
	*out_len = (uint32_t)sz.data_size;
	return ST_OK;
}

/* Find attribute (type, name) of `mref' and read it whole. */
int ntfs_attr_readall(ntfs_fs_t *fs, uint64_t mref, uint32_t type,
		      const uint16_t *name, uint32_t nlen, uint32_t cap,
		      uint8_t **out, uint32_t *out_len)
{
	ntfs_actx_t *c;
	int r;

	r = ntfs_actx_open(fs, mref, &c);
	if (r != ST_OK)
		return r;
	r = ntfs_actx_find(c, type, name ? name : ntfs_unnamed,
			   name ? nlen : 0, 0);
	while (r == ST_OK && c->a->non_resident &&
	       c->a->data.non_resident.lowest_vcn != 0)
		r = ntfs_actx_find(c, type, name ? name : ntfs_unnamed,
				   name ? nlen : 0, 0);
	if (r == ST_OK)
		r = ntfs_attr_readall_ctx(c, cap, out, out_len);
	ntfs_actx_close(c);
	return r;
}

/* ===========================================================================
 * S27  Editable inodes: a file's attribute set, rebuilt and committed
 *
 * Every change of a file's MFT record(s) goes through an "editable inode"
 * (ntfs_eino_t): the file's whole attribute set is loaded into memory --
 * every attribute record copied out of the base record and, through the
 * attribute list, out of its extension records (validated by the same
 * walk the readers use, S8) -- changed there (attributes added, removed,
 * resident values replaced, non-resident attributes given a new runlist
 * and sizes), and then COMMITTED: laid out into records again and
 * written.
 *
 * Layout (ntfs_eino_commit()):
 *   - everything fits into the base record: one record, no attribute list
 *     (an old list and its extension records are dropped after the base
 *     record was written);
 *   - else: an $ATTRIBUTE_LIST (resident when it fits next to
 *     $STANDARD_INFORMATION, else non-resident in fresh clusters, written
 *     whole clusters before it is referenced) in the base record,
 *     $STANDARD_INFORMATION always in the base record, then the base record
 *     filled with what was there before, the $FILE_NAMEs and the index
 *     root, in that preference; extension records whose attributes
 *     did not change are kept as they are; every changed or new attribute
 *     that does not fit the base record goes into FRESH extension records.
 *     The base record is the commit point: written last, after the new
 *     extension records (and a barrier), it switches from the old layout
 *     to the new one in one record write; old extension records and old
 *     attribute-list clusters are freed only after it (and a barrier), so
 *     a power cut leaves at worst unreferenced records / clusters (a leak
 *     chkdsk reclaims), never a reference to something freed.
 *   - system records (below 16) never get an attribute list; one that has
 *     one is rewritten in place, every attribute staying in its record
 *     ($MFT's own extension records must stay readable).
 * Attribute instances: kept where the attribute stays in its record,
 * fresh (next_attr_instance) where it moves into the base record, numbered
 * from 0 in a fresh extension record.  Attributes are ordered by type,
 * name (upcase collation, then exact) and lowest VCN, in the records and
 * in the list.
 *
 * Non-resident attributes are split into extents whose mapping pairs fit
 * an extension record (S26 encoder), so no attribute is ever too large to
 * place.  Clusters that a change frees (a non-resident value replaced by
 * a resident one, an old copy-on-write value) are listed in `fafter' and
 * freed only after the commit; clusters allocated for the change are
 * listed in `fundo' and freed if the commit fails, so a failed commit
 * leaves the volume as it was (all-or-nothing).
 * =========================================================================== */

static inline int64_t ntfs_ea_lowest(const ntfs_eattr_t *x)
{
	const ntfs_attr_record_t *a = ntfs_ea_hdr(x);

	return a->non_resident ? a->data.non_resident.lowest_vcn : 0;
}

/* Offset of the first attribute in a record with update sequence array
 * at `usa_ofs', and the space for attributes (the end marker excluded). */
static uint32_t ntfs_rec_attrs_off(const ntfs_fs_t *fs, uint16_t usa_ofs)
{
	uint32_t cnt = fs->rec_size / NTFS_BLOCK_SIZE + 1;

	return ((uint32_t)usa_ofs + 2U * cnt + 7U) & ~7U;
}

static uint32_t ntfs_rec_cap(const ntfs_fs_t *fs, uint16_t usa_ofs)
{
	return fs->rec_size - ntfs_rec_attrs_off(fs, usa_ofs) - 8U;
}

void ntfs_eino_free(ntfs_eino_t *e)
{
	if (!e)
		return;
	for (uint32_t i = 0; i < e->n; i++)
		ntfs_free(e->a[i].b);
	ntfs_free(e->a);
	ntfs_free(e->x);
	ntfs_rl_free(&e->al_rl);
	ntfs_free(e->fafter);
	ntfs_free(e->fundo);
	kfree(e);
}

static int ntfs_eino_add_raw(ntfs_eino_t *e, uint8_t *b, uint64_t orec,
			     uint16_t oinst, int dirty)
{
	if (e->n == e->cap) {
		uint32_t nc = e->cap ? e->cap * 2 : 16;
		ntfs_eattr_t *na;

		if (nc > NTFS_MAX_EXTENTS * 2)
			return ST_NOMEM;
		na = (ntfs_eattr_t *)ntfs_zalloc(nc * sizeof(*na));
		if (!na)
			return ST_NOMEM;
		if (e->n)
			mm_memcpy(na, e->a, e->n * sizeof(*na));
		ntfs_free(e->a);
		e->a = na;
		e->cap = nc;
	}
	mm_memset(&e->a[e->n], 0, sizeof(e->a[0]));
	e->a[e->n].b = b;
	e->a[e->n].orec = orec;
	e->a[e->n].oinst = oinst;
	e->a[e->n].dirty = (uint8_t)dirty;
	e->n++;
	return ST_OK;
}

static int ntfs_eino_add_ext(ntfs_eino_t *e, uint64_t no, const uint8_t *rec)
{
	const ntfs_mft_record_t *m = (const ntfs_mft_record_t *)rec;

	for (uint32_t i = 0; i < e->nx; i++)
		if (e->x[i].no == no) {
			e->x[i].nattr++;
			return ST_OK;
		}
	if (e->nx == e->xcap) {
		uint32_t nc = e->xcap ? e->xcap * 2 : 8;
		ntfs_eext_t *nx;

		if (nc > NTFS_MAX_EXTENTS)
			return ST_NOMEM;
		nx = (ntfs_eext_t *)ntfs_zalloc(nc * sizeof(*nx));
		if (!nx)
			return ST_NOMEM;
		if (e->nx)
			mm_memcpy(nx, e->x, e->nx * sizeof(*nx));
		ntfs_free(e->x);
		e->x = nx;
		e->xcap = nc;
	}
	e->x[e->nx].no = no;
	e->x[e->nx].seq = m->sequence_number;
	e->x[e->nx].usa_ofs = m->usa_ofs;
	e->x[e->nx].usn = ntfs_le16(rec + m->usa_ofs);
	e->x[e->nx].next_inst = m->next_attr_instance;
	e->x[e->nx].lsn = m->lsn;
	e->x[e->nx].keep = 0;
	e->x[e->nx].nattr = 1;
	e->nx++;
	return ST_OK;
}

/* Load the attribute set of file `mref' (base record; a stale
 * reference is ST_IO). */
int ntfs_eino_load(ntfs_fs_t *fs, uint64_t mref, ntfs_eino_t **out)
{
	ntfs_actx_t *c = NULL;
	ntfs_eino_t *e;
	const ntfs_mft_record_t *m;
	uint32_t from_base = 0, in_base = 0;
	int r;

	*out = NULL;
	e = (ntfs_eino_t *)ntfs_zalloc(sizeof(*e));
	if (!e)
		return ST_NOMEM;
	e->fs = fs;
	r = ntfs_actx_open(fs, mref, &c);
	if (r != ST_OK)
		goto fail;
	m = (const ntfs_mft_record_t *)c->base;
	e->no = c->base_no;
	e->seq = m->sequence_number;
	e->link_count = m->link_count;
	e->rflags = m->flags;
	e->next_inst = m->next_attr_instance;
	e->usa_ofs = m->usa_ofs;
	e->usn = ntfs_le16(c->base + m->usa_ofs);
	e->lsn = m->lsn;
	e->had_al = c->al != NULL;
	/* The attribute list itself (its clusters are freed when it is
	 * replaced) and the count of attributes physically in the base. */
	for (uint32_t off = m->attrs_offset; off + 4 <= m->bytes_in_use;) {
		const ntfs_attr_record_t *a =
			(const ntfs_attr_record_t *)(c->base + off);

		if (a->type == NTFS_AT_END)
			break;
		if (ntfs_attr_valid(fs, c->base, off, e->no) != ST_OK) {
			r = ST_IO;
			goto fail;
		}
		if (a->type == NTFS_AT_ATTRIBUTE_LIST) {
			if (a->non_resident) {
				e->al_nonres = 1;
				r = ntfs_mp_decode(fs, a, a->length, &e->al_rl);
				if (r != ST_OK)
					goto fail;
			}
		} else {
			in_base++;
		}
		off += a->length;
	}
	for (;;) {
		const ntfs_attr_record_t *a;
		uint8_t *b;

		r = ntfs_actx_find(c, NTFS_AT_UNUSED, NULL, 0, 0);
		if (r == ST_NOT_FOUND)
			break;
		if (r != ST_OK)
			goto fail;
		a = c->a;
		if (a->type == NTFS_AT_ATTRIBUTE_LIST)
			continue;
		b = (uint8_t *)kalloc(a->length);
		if (!b) {
			r = ST_NOMEM;
			goto fail;
		}
		mm_memcpy(b, a, a->length);
		r = ntfs_eino_add_raw(e, b, c->rec_no, a->instance, 0);
		if (r != ST_OK) {
			kfree(b);
			goto fail;
		}
		if (c->rec_no == e->no) {
			from_base++;
		} else {
			r = ntfs_eino_add_ext(e, c->rec_no, c->ext);
			if (r != ST_OK)
				goto fail;
		}
	}
	/* Every attribute of the base record must be in the list. */
	if (e->had_al && from_base != in_base) {
		WARN_RATELIMIT(1, "ntfs: MFT record %llu: attribute list does "
				  "not list every attribute",
			       (unsigned long long)e->no);
		r = ST_IO;
		goto fail;
	}
	ntfs_actx_close(c);
	*out = e;
	return ST_OK;
fail:
	ntfs_actx_close(c);
	ntfs_eino_free(e);
	return r;
}

/* An empty attribute set for a new record `no' (just allocated). */
ntfs_eino_t *ntfs_eino_new(ntfs_fs_t *fs, uint64_t no, uint16_t seq,
			   uint16_t rflags)
{
	ntfs_eino_t *e = (ntfs_eino_t *)ntfs_zalloc(sizeof(*e));

	if (!e)
		return NULL;
	e->fs = fs;
	e->no = no;
	e->seq = seq ? seq : 1;
	e->rflags = (uint16_t)(rflags | NTFS_MFT_REC_IN_USE);
	e->usa_ofs = fs->new_usa_ofs;
	e->usn = 1;
	e->is_new = 1;
	return e;
}

/* Attribute order: type, name (upcase, then exact), lowest VCN. */
static int ntfs_eattr_cmp(ntfs_fs_t *fs, const ntfs_eattr_t *x,
			  const ntfs_eattr_t *y)
{
	const ntfs_attr_record_t *a = ntfs_ea_hdr(x), *b = ntfs_ea_hdr(y);
	int c;
	int64_t la, lb;

	if (a->type != b->type)
		return a->type < b->type ? -1 : 1;
	c = ntfs_collate_names(fs, ntfs_attr_name(a), a->name_length,
			       ntfs_attr_name(b), b->name_length, 0);
	if (c)
		return c;
	la = ntfs_ea_lowest(x);
	lb = ntfs_ea_lowest(y);
	return la < lb ? -1 : (la > lb ? 1 : 0);
}

static void ntfs_eino_sort(ntfs_eino_t *e)
{
	for (uint32_t i = 1; i < e->n; i++) {
		ntfs_eattr_t k = e->a[i];
		uint32_t j = i;

		while (j > 0 && ntfs_eattr_cmp(e->fs, &e->a[j - 1], &k) > 0) {
			e->a[j] = e->a[j - 1];
			j--;
		}
		e->a[j] = k;
	}
}

/* Index of the next attribute (type, name) at or after `from' (name NULL:
 * any name), or -1. */
long ntfs_eino_find(const ntfs_eino_t *e, uint32_t type,
		    const uint16_t *name, uint32_t nlen, uint32_t from)
{
	for (uint32_t i = from; i < e->n; i++) {
		const ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);

		if (a->type != type)
			continue;
		if (name && (a->name_length != nlen ||
			     (nlen && kmemcmp(ntfs_attr_name(a), name,
					      nlen * 2U) != 0)))
			continue;
		return (long)i;
	}
	return -1;
}

void ntfs_eino_remove(ntfs_eino_t *e, uint32_t idx)
{
	ntfs_free(e->a[idx].b);
	for (uint32_t i = idx + 1; i < e->n; i++)
		e->a[i - 1] = e->a[i];
	e->n--;
}

/* Note clusters to free after the commit / on failure. */
static int ntfs_eino_note(ntfs_run_t **arr, uint32_t *n, uint32_t *cap,
			  int64_t lcn, int64_t len)
{
	return ntfs_runs_add(arr, n, cap, lcn, len);
}

/* The runlist of non-resident attribute (type, name) of the set, all
 * extents; *first gets the index of the first extent. */
static int ntfs_eino_runlist(ntfs_eino_t *e, uint32_t type,
			     const uint16_t *name, uint32_t nlen,
			     ntfs_runlist_t *rl, long *first)
{
	long i;
	int r;

	mm_memset(rl, 0, sizeof(*rl));
	*first = -1;
	for (i = ntfs_eino_find(e, type, name, nlen, 0); i >= 0;
	     i = ntfs_eino_find(e, type, name, nlen, (uint32_t)i + 1)) {
		ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);

		if (!a->non_resident)
			return ST_IO;
		if (*first < 0)
			*first = i;
		r = ntfs_mp_decode(e->fs, a, a->length, rl);
		if (r != ST_OK) {
			ntfs_rl_free(rl);
			return r;
		}
	}
	return *first < 0 ? ST_NOT_FOUND : ST_OK;
}

/* Remove every extent of (type, name); with `free_clusters' a
 * non-resident one's clusters are noted to be freed after the commit. */
int ntfs_eino_drop(ntfs_eino_t *e, uint32_t type, const uint16_t *name,
		   uint32_t nlen, int free_clusters)
{
	long i = ntfs_eino_find(e, type, name, nlen, 0);

	if (i < 0)
		return ST_OK;
	if (free_clusters && ntfs_ea_hdr(&e->a[i])->non_resident) {
		ntfs_runlist_t rl;
		long first;
		int r = ntfs_eino_runlist(e, type, name, nlen, &rl, &first);

		if (r != ST_OK)
			return r;
		for (uint32_t k = 0; k < rl.count && r == ST_OK; k++)
			if (rl.runs[k].lcn != NTFS_LCN_HOLE)
				r = ntfs_eino_note(&e->fafter, &e->nfafter,
						   &e->cfafter, rl.runs[k].lcn,
						   rl.runs[k].len);
		ntfs_rl_free(&rl);
		if (r != ST_OK)
			return r;
	}
	while ((i = ntfs_eino_find(e, type, name, nlen, 0)) >= 0)
		ntfs_eino_remove(e, (uint32_t)i);
	return ST_OK;
}

/* A resident attribute record. */
uint8_t *ntfs_ab_res(uint32_t type, const uint16_t *name, uint32_t nlen,
		     uint16_t flags, const void *val, uint32_t vlen,
		     uint8_t rflags)
{
	uint32_t voff = (NTFS_ATTR_RESIDENT_HDR + 2U * nlen + 7U) & ~7U;
	uint32_t len = (voff + vlen + 7U) & ~7U;
	uint8_t *b = (uint8_t *)ntfs_zalloc(len);
	ntfs_attr_record_t *a = (ntfs_attr_record_t *)b;

	if (!b)
		return NULL;
	a->type = type;
	a->length = len;
	a->non_resident = 0;
	a->name_length = (uint8_t)nlen;
	a->name_offset = nlen ? NTFS_ATTR_RESIDENT_HDR : 0;
	a->flags = flags;
	a->data.resident.value_length = vlen;
	a->data.resident.value_offset = (uint16_t)voff;
	a->data.resident.flags = rflags;
	if (nlen)
		mm_memcpy(b + NTFS_ATTR_RESIDENT_HDR, name, 2U * nlen);
	if (vlen)
		mm_memcpy(b + voff, val, vlen);
	return b;
}

/* A non-resident attribute record (one extent). */
static uint8_t *ntfs_ab_nonres(uint32_t type, const uint16_t *name,
			       uint32_t nlen, uint16_t flags, int64_t lowest,
			       int64_t highest, uint8_t cu, uint64_t alloc,
			       uint64_t data, uint64_t init, uint64_t comp,
			       const uint8_t *mp, uint32_t mplen)
{
	uint32_t hdr = ((flags & (NTFS_ATTR_COMPRESSION_MASK |
				  NTFS_ATTR_IS_SPARSE)) &&
			lowest == 0) ?
			       NTFS_ATTR_NONRES_HDR_COMP :
			       NTFS_ATTR_NONRES_HDR;
	uint32_t mpo = (hdr + 2U * nlen + 7U) & ~7U;
	uint32_t len = (mpo + mplen + 7U) & ~7U;
	uint8_t *b = (uint8_t *)ntfs_zalloc(len);
	ntfs_attr_record_t *a = (ntfs_attr_record_t *)b;

	if (!b)
		return NULL;
	a->type = type;
	a->length = len;
	a->non_resident = 1;
	a->name_length = (uint8_t)nlen;
	a->name_offset = nlen ? (uint16_t)hdr : 0;
	a->flags = flags;
	a->data.non_resident.lowest_vcn = lowest;
	a->data.non_resident.highest_vcn = highest;
	a->data.non_resident.mapping_pairs_offset = (uint16_t)mpo;
	a->data.non_resident.compression_unit = cu;
	if (lowest == 0) {
		a->data.non_resident.allocated_size = (int64_t)alloc;
		a->data.non_resident.data_size = (int64_t)data;
		a->data.non_resident.initialized_size = (int64_t)init;
		if (hdr == NTFS_ATTR_NONRES_HDR_COMP)
			a->data.non_resident.compressed_size = (int64_t)comp;
	}
	if (nlen)
		mm_memcpy(b + hdr, name, 2U * nlen);
	mm_memcpy(b + mpo, mp, mplen);
	return b;
}

/* Insert attribute record `b' (ownership passes), keeping the old
 * placement / instance of the extent it replaces (same type, name and
 * lowest VCN) when `like' says so. */
int ntfs_eino_put(ntfs_eino_t *e, uint8_t *b, uint64_t orec,
		  uint16_t oinst)
{
	int r = ntfs_eino_add_raw(e, b, orec, oinst, 1);

	if (r != ST_OK)
		kfree(b);
	return r;
}

/* Set resident attribute (type, name) to `val' (replacing every extent of
 * it; a non-resident one's clusters are freed after the commit). */
int ntfs_eino_set_res(ntfs_eino_t *e, uint32_t type,
		      const uint16_t *name, uint32_t nlen,
		      uint16_t flags, const void *val, uint32_t vlen)
{
	long i = ntfs_eino_find(e, type, name, nlen, 0);
	uint64_t orec = ~0ULL;
	uint16_t oinst = 0;
	uint8_t *b;
	int r;

	if (i >= 0 && !ntfs_ea_hdr(&e->a[i])->non_resident) {
		orec = e->a[i].orec;
		oinst = e->a[i].oinst;
	}
	b = ntfs_ab_res(type, name, nlen, flags, val, vlen,
			type == NTFS_AT_FILE_NAME ? 1 : 0);
	if (!b)
		return ST_NOMEM;
	r = ntfs_eino_drop(e, type, name, nlen, 1);
	if (r != ST_OK) {
		kfree(b);
		return r;
	}
	return ntfs_eino_put(e, b, orec, oinst);
}

/* Most mapping-pair bytes one extent may carry: what an empty extension
 * record holds besides the header and the name. */
static uint32_t ntfs_ext_mp_cap(const ntfs_fs_t *fs, uint32_t nlen)
{
	uint32_t cap = ntfs_rec_cap(fs, fs->new_usa_ofs);
	uint32_t hdr = (NTFS_ATTR_NONRES_HDR_COMP + 2U * nlen + 7U) & ~7U;

	return cap > hdr + 16 ? (cap - hdr) & ~7U : 16;
}

/* Give non-resident attribute (type, name) runlist `rl' and the sizes:
 * every old extent is replaced by as many extents as the mapping pairs
 * need (an extent that starts where an old one started keeps its record
 * and instance).  The clusters are the caller's business (the runlist
 * usually shares them with the old one). */
int ntfs_eino_set_nonres(ntfs_eino_t *e, uint32_t type,
			 const uint16_t *name, uint32_t nlen,
			 uint16_t flags, uint8_t cu,
			 const ntfs_runlist_t *rl, uint64_t alloc,
			 uint64_t data, uint64_t init, uint64_t comp)
{
	struct ntfs_oldext {
		int64_t lowest;
		uint64_t orec;
		uint16_t oinst;
		int taken;
	} *old = NULL;
	uint32_t cap = ntfs_ext_mp_cap(e->fs, nlen), nold = 0;
	uint8_t *mp;
	int64_t v = 0, end = rl->end_vcn;
	long i;
	int r = ST_OK;

	if ((uint64_t)end << e->fs->cluster_bits != alloc ||
	    data > alloc || init > data) {
		WARN(1, "ntfs: runlist (%lld clusters) and sizes (%llu %llu "
			"%llu) disagree",
		     (long long)end, (unsigned long long)alloc,
		     (unsigned long long)data, (unsigned long long)init);
		return ST_INVALID;
	}
	mp = (uint8_t *)kalloc(cap);
	old = (struct ntfs_oldext *)ntfs_zalloc((e->n + 1) * sizeof(*old));
	if (!mp || !old) {
		ntfs_free(mp);
		ntfs_free(old);
		return ST_NOMEM;
	}
	for (i = ntfs_eino_find(e, type, name, nlen, 0); i >= 0;
	     i = ntfs_eino_find(e, type, name, nlen, (uint32_t)i + 1)) {
		if (!ntfs_ea_hdr(&e->a[i])->non_resident)
			continue;
		old[nold].lowest = ntfs_ea_lowest(&e->a[i]);
		old[nold].orec = e->a[i].orec;
		old[nold].oinst = e->a[i].oinst;
		nold++;
	}
	r = ntfs_eino_drop(e, type, name, nlen, 0);
	while (r == ST_OK) {
		int64_t stop;
		uint32_t used;
		uint8_t *b;
		uint64_t orec = ~0ULL;
		uint16_t oinst = 0;

		used = ntfs_mp_encode(rl, v, end, mp, cap, &stop);
		if (!used || (stop == v && v < end)) {
			r = ST_NOSPC;
			break;
		}
		for (uint32_t k = 0; k < nold; k++)
			if (!old[k].taken && old[k].lowest == v) {
				orec = old[k].orec;
				oinst = old[k].oinst;
				old[k].taken = 1;
				break;
			}
		b = ntfs_ab_nonres(type, name, nlen, flags, v, stop - 1, cu,
				   alloc, data, init, comp, mp, used);
		if (!b) {
			r = ST_NOMEM;
			break;
		}
		r = ntfs_eino_put(e, b, orec, oinst);
		v = stop;
		if (v >= end)
			break;
	}
	kfree(mp);
	kfree(old);
	return r;
}

/* Read the value of attribute (type, name) of the set (resident or not),
 * at most `cap' bytes.  ST_NOT_FOUND when absent. */
int ntfs_eino_value(ntfs_eino_t *e, uint32_t type, const uint16_t *name,
		    uint32_t nlen, uint32_t cap, uint8_t **out,
		    uint32_t *out_len)
{
	long i = ntfs_eino_find(e, type, name, nlen, 0);
	ntfs_attr_record_t *a;
	ntfs_runlist_t rl;
	uint64_t ds, is;
	uint8_t *buf;
	long first;
	int r;

	*out = NULL;
	*out_len = 0;
	if (i < 0)
		return ST_NOT_FOUND;
	a = ntfs_ea_hdr(&e->a[i]);
	if (!a->non_resident) {
		uint32_t vl = a->data.resident.value_length;

		if (vl > cap)
			return ST_IO;
		buf = (uint8_t *)kalloc(vl ? vl : 1);
		if (!buf)
			return ST_NOMEM;
		mm_memcpy(buf, ntfs_attr_value(a), vl);
		*out = buf;
		*out_len = vl;
		return ST_OK;
	}
	if (a->flags & (NTFS_ATTR_COMPRESSION_MASK | NTFS_ATTR_IS_ENCRYPTED))
		return ST_UNSUPPORTED;
	r = ntfs_eino_runlist(e, type, name, nlen, &rl, &first);
	if (r != ST_OK)
		return r;
	a = ntfs_ea_hdr(&e->a[first]);
	ds = (uint64_t)a->data.non_resident.data_size;
	is = (uint64_t)a->data.non_resident.initialized_size;
	if (ds > cap || ((uint64_t)rl.end_vcn << e->fs->cluster_bits) < ds) {
		ntfs_rl_free(&rl);
		return ST_IO;
	}
	buf = (uint8_t *)kalloc(ds ? ds : 1);
	if (!buf) {
		ntfs_rl_free(&rl);
		return ST_NOMEM;
	}
	r = ntfs_rl_pread(e->fs, &rl, 0, ds, is, buf);
	ntfs_rl_free(&rl);
	if (r != ST_OK) {
		kfree(buf);
		return r;
	}
	*out = buf;
	*out_len = (uint32_t)ds;
	return ST_OK;
}

/* Set the value of (type, name) to `val': resident when it is at most
 * `res_max' bytes, else non-resident in FRESH clusters written in whole
 * clusters (copy-on-write: the old clusters are freed after the commit,
 * the new ones if it fails). */
int ntfs_eino_set_value(ntfs_eino_t *e, uint32_t type,
			const uint16_t *name, uint32_t nlen,
			const uint8_t *val, uint32_t vlen,
			uint32_t res_max)
{
	ntfs_fs_t *fs = e->fs;
	ntfs_runlist_t rl;
	ntfs_run_t *runs = NULL;
	uint32_t nruns = 0;
	uint64_t clusters, bytes;
	uint8_t *pad;
	int r;

	if (vlen <= res_max)
		return ntfs_eino_set_res(e, type, name, nlen, 0, val, vlen);
	clusters = ((uint64_t)vlen + fs->cluster_size - 1) >> fs->cluster_bits;
	bytes = clusters << fs->cluster_bits;
	r = ntfs_clusters_alloc(fs, -1, clusters, NTFS_AL_META, &runs, &nruns);
	if (r != ST_OK)
		return r;
	mm_memset(&rl, 0, sizeof(rl));
	for (uint32_t i = 0; i < nruns && r == ST_OK; i++) {
		r = ntfs_eino_note(&e->fundo, &e->nfundo, &e->cfundo,
				   runs[i].lcn, runs[i].len);
		if (r != ST_OK) {
			/* runs i.. are not on the undo list: given back here
			 * (nothing refers to them) */
			for (uint32_t j = i; j < nruns; j++)
				ntfs_clusters_free(fs, runs[j].lcn,
						   (uint64_t)runs[j].len);
			break;
		}
		r = ntfs_rl_append(&rl, rl.end_vcn, runs[i].lcn, runs[i].len);
	}
	ntfs_free(runs);
	pad = r == ST_OK ? (uint8_t *)ntfs_zalloc(bytes) : NULL;
	if (r == ST_OK && !pad)
		r = ST_NOMEM;
	if (r == ST_OK) {
		mm_memcpy(pad, val, vlen);
		/* Whole clusters, the final partial one included. */
		r = ntfs_rl_pwrite_raw(fs, &rl, 0, pad, bytes);
	}
	ntfs_free(pad);
	if (r == ST_OK)
		r = ntfs_eino_drop(e, type, name, nlen, 1);
	if (r == ST_OK)
		r = ntfs_eino_set_nonres(e, type, name, nlen, 0, 0, &rl, bytes,
					 vlen, vlen, 0);
	ntfs_rl_free(&rl);
	return r;
}

/* ---- commit ---- */

/* Build record `slot' of the layout into `buf' (rec_size bytes). */
static void ntfs_eino_build(ntfs_eino_t *e, uint32_t slot, uint8_t *buf,
			    uint64_t no, uint16_t seq, uint16_t usa_ofs,
			    uint16_t usn, uint64_t lsn, uint16_t next_inst,
			    int is_base)
{
	ntfs_fs_t *fs = e->fs;
	ntfs_mft_record_t *m = (ntfs_mft_record_t *)buf;
	uint32_t off = ntfs_rec_attrs_off(fs, usa_ofs);

	mm_memset(buf, 0, fs->rec_size);
	m->magic = NTFS_MAGIC_FILE;
	m->usa_ofs = usa_ofs;
	m->usa_count = (uint16_t)(fs->rec_size / NTFS_BLOCK_SIZE + 1);
	m->lsn = lsn;
	m->sequence_number = seq;
	m->attrs_offset = (uint16_t)off;
	m->bytes_allocated = fs->rec_size;
	if (is_base) {
		m->link_count = e->link_count;
		m->flags = e->rflags | NTFS_MFT_REC_IN_USE;
		m->base_mft_record = 0;
	} else {
		m->link_count = 0;
		m->flags = NTFS_MFT_REC_IN_USE;
		m->base_mft_record = NTFS_MAKE_MREF(e->no, e->seq);
	}
	m->next_attr_instance = next_inst;
	if (usa_ofs >= NTFS_MFT_REC_HDR)
		m->mft_record_number = (uint32_t)no;
	ntfs_put_le16(buf + usa_ofs, usn ? usn : 1);
	for (uint32_t i = 0; i < e->n; i++) {
		ntfs_eattr_t *x = &e->a[i];
		ntfs_attr_record_t *a;

		if (x->slot != slot)
			continue;
		a = ntfs_ea_hdr(x);
		mm_memcpy(buf + off, x->b, a->length);
		((ntfs_attr_record_t *)(buf + off))->instance = x->inst;
		off += a->length;
	}
	ntfs_put_le32(buf + off, NTFS_AT_END);
	m->bytes_in_use = off + 8;
}

/* Free what the commit replaced (after it), or what it allocated (when it
 * failed). */
static void ntfs_eino_release(ntfs_eino_t *e, int ok)
{
	ntfs_run_t *arr = ok ? e->fafter : e->fundo;
	uint32_t n = ok ? e->nfafter : e->nfundo;

	if (n && ok && ntfs_barrier(e->fs) != ST_OK) {
		ntfs_leaked(e->fs); /* not durable: leak rather than free */
		return;
	}
	for (uint32_t i = 0; i < n; i++)
		ntfs_clusters_free(e->fs, arr[i].lcn, (uint64_t)arr[i].len);
	if (ok) {
		e->nfafter = 0;
		e->nfundo = 0;
	} else {
		e->nfundo = 0;
	}
}

/* Instances for the attributes placed in the base record: an attribute
 * that stays in the base record keeps its instance (unless another kept
 * one has it already), every other one gets the next free number.
 * Returns the record's next_attr_instance. */
static uint16_t ntfs_eino_base_instances(ntfs_eino_t *e, uint16_t next)
{
	for (uint32_t i = 0; i < e->n; i++) {
		ntfs_eattr_t *x = &e->a[i];
		int keep = x->slot == 0 && x->orec == e->no;

		if (x->slot != 0)
			continue;
		for (uint32_t j = 0; keep && j < e->n; j++)
			if (j != i && e->a[j].slot == 0 &&
			    e->a[j].orec == e->no && e->a[j].oinst == x->oinst &&
			    j < i)
				keep = 0;
		x->inst = keep ? x->oinst : 0xFFFF;
	}
	for (uint32_t i = 0; i < e->n; i++)
		if (e->a[i].slot == 0 && e->a[i].inst != 0xFFFF &&
		    e->a[i].inst >= next)
			next = (uint16_t)(e->a[i].inst + 1);
	for (uint32_t i = 0; i < e->n; i++)
		if (e->a[i].slot == 0 && e->a[i].inst == 0xFFFF)
			e->a[i].inst = next++;
	return next;
}

/* The bytes of the attribute list for the layout; `slot_no' / `slot_seq'
 * give the fresh extension records. */
static void ntfs_eino_build_al(ntfs_eino_t *e, uint8_t *al,
			       const uint64_t *slot_no,
			       const uint16_t *slot_seq)
{
	uint32_t pos = 0;

	for (uint32_t i = 0; i < e->n; i++) {
		ntfs_eattr_t *x = &e->a[i];
		ntfs_attr_record_t *a = ntfs_ea_hdr(x);
		ntfs_attr_list_entry_t *le = (ntfs_attr_list_entry_t *)(al + pos);
		uint32_t el = (NTFS_ALE_HDR + 2U * a->name_length + 7U) & ~7U;
		uint64_t rno;
		uint16_t rseq;

		if (x->slot == 0) {
			rno = e->no;
			rseq = e->seq;
		} else if (x->slot <= e->nx) {
			rno = e->x[x->slot - 1].no;
			rseq = e->x[x->slot - 1].seq;
		} else {
			rno = slot_no[x->slot - e->nx - 1];
			rseq = slot_seq[x->slot - e->nx - 1];
		}
		mm_memset(le, 0, el);
		le->type = a->type;
		le->length = (uint16_t)el;
		le->name_length = a->name_length;
		le->name_offset = NTFS_ALE_HDR;
		le->lowest_vcn = ntfs_ea_lowest(x);
		le->mft_reference = NTFS_MAKE_MREF(rno, rseq);
		le->instance = x->inst;
		if (a->name_length)
			mm_memcpy(al + pos + NTFS_ALE_HDR, ntfs_attr_name(a),
				  2U * a->name_length);
		pos += el;
	}
}

/* Mapping-pair bytes of a non-resident attribute list (one extent); the
 * layout reserves its header plus this much in the base record. */
#define NTFS_AL_MP_MAX 64U

/* A non-resident attribute list in fresh clusters (whole clusters
 * written); the clusters are noted for the undo. */
static uint8_t *ntfs_eino_al_nonres(ntfs_eino_t *e, const uint8_t *al,
				    uint32_t al_len, int *err)
{
	ntfs_fs_t *fs = e->fs;
	ntfs_run_t *runs = NULL;
	uint32_t nr = 0, used = 0;
	uint64_t cl = ((uint64_t)al_len + fs->cluster_size - 1) >>
		      fs->cluster_bits;
	ntfs_runlist_t rl;
	uint8_t mp[NTFS_AL_MP_MAX], *pad = NULL, *b = NULL;
	int64_t stop = 0;
	int r;

	mm_memset(&rl, 0, sizeof(rl));
	r = ntfs_clusters_alloc(fs, -1, cl, NTFS_AL_META, &runs, &nr);
	for (uint32_t k = 0; r == ST_OK && k < nr; k++) {
		r = ntfs_eino_note(&e->fundo, &e->nfundo, &e->cfundo,
				   runs[k].lcn, runs[k].len);
		if (r != ST_OK) {
			/* runs k.. are not on the undo list: given back
			 * here (nothing refers to them) */
			for (uint32_t j = k; j < nr; j++)
				ntfs_clusters_free(fs, runs[j].lcn,
						   (uint64_t)runs[j].len);
			break;
		}
		r = ntfs_rl_append(&rl, rl.end_vcn, runs[k].lcn, runs[k].len);
	}
	ntfs_free(runs);
	if (r == ST_OK) {
		pad = (uint8_t *)ntfs_zalloc(cl << fs->cluster_bits);
		if (!pad)
			r = ST_NOMEM;
	}
	if (r == ST_OK) {
		mm_memcpy(pad, al, al_len);
		r = ntfs_rl_pwrite_raw(fs, &rl, 0, pad, cl << fs->cluster_bits);
	}
	if (r == ST_OK) {
		used = ntfs_mp_encode(&rl, 0, rl.end_vcn, mp, sizeof(mp), &stop);
		if (stop != rl.end_vcn)
			r = ST_NOSPC; /* too fragmented for one extent */
	}
	if (r == ST_OK) {
		b = ntfs_ab_nonres(NTFS_AT_ATTRIBUTE_LIST, NULL, 0, 0, 0,
				   (int64_t)cl - 1, 0, cl << fs->cluster_bits,
				   al_len, al_len, 0, mp, used);
		if (!b)
			r = ST_NOMEM;
	}
	ntfs_free(pad);
	ntfs_rl_free(&rl);
	*err = r;
	return b;
}

/* Lay the set out and write it (see the section comment).  The set is
 * spent afterwards (ntfs_eino_free()). */
int ntfs_eino_commit(ntfs_eino_t *e)
{
	ntfs_fs_t *fs = e->fs;
	uint32_t base_cap = ntfs_rec_cap(fs, e->usa_ofs);
	uint32_t ext_cap = ntfs_rec_cap(fs, fs->new_usa_ofs);
	uint32_t total = 0, nslots = 0, nalloc = 0, *slot_used = NULL;
	uint64_t *slot_no = NULL;
	uint16_t *slot_seq = NULL, *slot_inst = NULL;
	uint8_t *buf = NULL, *al = NULL, *alb = NULL;
	uint32_t al_len = 0, base_free, al_res_size;
	uint16_t next_inst;
	int r, al_res, system = e->no < NTFS_FILE_FIRST_USER;

	ntfs_assert_meta_excl(fs);
	r = ntfs_write_begin(fs);
	if (r != ST_OK)
		goto out_fail;
	ntfs_eino_sort(e);
	for (uint32_t i = 0; i < e->n; i++)
		total += ntfs_ea_hdr(&e->a[i])->length;
	buf = (uint8_t *)kalloc(fs->rec_size);
	if (!buf) {
		r = ST_NOMEM;
		goto out_fail;
	}
	if (system && e->had_al) {
		/* System records are never re-laid out (ntfs_eino_commit_mft()
		 * changes $MFT's own records in place). */
		r = ST_NOSPC;
		goto out_fail;
	}
	if (total <= base_cap) {
		/* ---- everything in the base record ---- */
		for (uint32_t i = 0; i < e->n; i++)
			e->a[i].slot = 0;
		next_inst = ntfs_eino_base_instances(e, e->next_inst);
		r = ntfs_barrier(fs);
		if (r != ST_OK)
			goto out_fail;
		ntfs_eino_build(e, 0, buf, e->no, e->seq, e->usa_ofs, e->usn,
				e->lsn, next_inst, 1);
		r = ntfs_mft_write(fs, e->no, buf);
		if (r != ST_OK)
			goto out_fail;
		/* The base record is on the device: the update is committed
		 * from here on.  The old extension records and list are
		 * unreferenced now; a failure to free them is a leak (the
		 * volume stays dirty, a check reclaims it), never a reason to
		 * report the commit as failed -- callers would undo a change
		 * the device already shows. */
		if (e->nx || e->al_nonres)
			r = ntfs_barrier(fs);
		for (uint32_t k = 0; r == ST_OK && k < e->nx; k++)
			r = ntfs_mft_free(fs, e->x[k].no);
		if (r == ST_OK && e->al_nonres)
			r = ntfs_rl_free_clusters(fs, &e->al_rl, 0,
						  e->al_rl.end_vcn);
		if (r != ST_OK)
			ntfs_fs_error(fs, e->no, "records or clusters left "
						 "behind by a commit leaked");
		r = ST_OK;
		e->nx = 0;
		e->al_nonres = 0;
		goto out_ok;
	}
	if (system) {
		r = ST_NOSPC; /* no attribute list for a system record */
		goto out_fail;
	}
	/* ---- several records with an attribute list ---- */
	for (uint32_t i = 0; i < e->n; i++) {
		ntfs_attr_record_t *a = ntfs_ea_hdr(&e->a[i]);

		al_len += (NTFS_ALE_HDR + 2U * a->name_length + 7U) & ~7U;
		e->a[i].slot = ~0U;
	}
	if (al_len > NTFS_ATTR_LIST_MAX) {
		r = ST_NOSPC;
		goto out_fail;
	}
	base_free = base_cap;
	for (uint32_t i = 0; i < e->n; i++)
		if (ntfs_ea_hdr(&e->a[i])->type ==
		    NTFS_AT_STANDARD_INFORMATION) {
			uint32_t l = ntfs_ea_hdr(&e->a[i])->length;

			if (l > base_free) {
				r = ST_NOSPC;
				goto out_fail;
			}
			e->a[i].slot = 0;
			base_free -= l;
		}
	/* The list: resident when it fits comfortably, else a non-resident
	 * record: its header and at most NTFS_AL_MP_MAX bytes of mapping
	 * pairs (ntfs_eino_al_nonres()). */
	al_res_size = (NTFS_ATTR_RESIDENT_HDR + al_len + 7U) & ~7U;
	al_res = al_res_size <= base_free && al_res_size <= base_cap / 2;
	if (al_res) {
		base_free -= al_res_size;
	} else if (base_free >= NTFS_ATTR_NONRES_HDR + NTFS_AL_MP_MAX) {
		base_free -= NTFS_ATTR_NONRES_HDR + NTFS_AL_MP_MAX;
	} else {
		r = ST_NOSPC;
		goto out_fail;
	}
	/* Extension records whose attributes are all unchanged stay -- all
	 * of them still in the set: a record that held an attribute removed
	 * since the load is rewritten (kept, it would still carry the
	 * attribute, outside the list and over clusters freed with it). */
	for (uint32_t k = 0; k < e->nx; k++) {
		uint32_t cnt = 0;
		int keep = 1;

		for (uint32_t i = 0; i < e->n; i++)
			if (e->a[i].orec == e->x[k].no) {
				cnt++;
				if (e->a[i].dirty || e->a[i].slot == 0)
					keep = 0;
			}
		e->x[k].keep = cnt && keep && cnt == e->x[k].nattr;
		if (e->x[k].keep)
			for (uint32_t i = 0; i < e->n; i++)
				if (e->a[i].orec == e->x[k].no) {
					e->a[i].slot = k + 1;
					e->a[i].inst = e->a[i].oinst;
				}
	}
	/* Fill the base record: what was there, the $FILE_NAMEs and the
	 * index root, then anything else in order. */
	for (int pass = 0; pass < 3; pass++)
		for (uint32_t i = 0; i < e->n; i++) {
			ntfs_eattr_t *x = &e->a[i];
			uint32_t t = ntfs_ea_hdr(x)->type;
			uint32_t l = ntfs_ea_hdr(x)->length;

			if (x->slot != ~0U)
				continue;
			if (pass == 0 && x->orec != e->no)
				continue;
			if (pass == 1 && t != NTFS_AT_FILE_NAME &&
			    t != NTFS_AT_INDEX_ROOT)
				continue;
			if (l <= base_free) {
				x->slot = 0;
				base_free -= l;
			}
		}
	/* The rest into fresh extension records, first fit. */
	slot_used = (uint32_t *)ntfs_zalloc((e->n + 1) * sizeof(uint32_t));
	if (!slot_used) {
		r = ST_NOMEM;
		goto out_fail;
	}
	for (uint32_t i = 0; i < e->n; i++) {
		ntfs_eattr_t *x = &e->a[i];
		uint32_t l = ntfs_ea_hdr(x)->length, s;

		if (x->slot != ~0U)
			continue;
		if (l > ext_cap) {
			WARN(1, "ntfs: attribute of %u bytes cannot be placed",
			     l);
			r = ST_NOSPC;
			goto out_fail;
		}
		for (s = 0; s < nslots; s++)
			if (slot_used[s] + l <= ext_cap)
				break;
		if (s == nslots)
			nslots++;
		slot_used[s] += l;
		x->slot = e->nx + 1 + s;
	}
	slot_no = (uint64_t *)ntfs_zalloc((nslots + 1) * sizeof(uint64_t));
	slot_seq = (uint16_t *)ntfs_zalloc((nslots + 1) * sizeof(uint16_t));
	slot_inst = (uint16_t *)ntfs_zalloc((nslots + 1) * sizeof(uint16_t));
	if (!slot_no || !slot_seq || !slot_inst) {
		r = ST_NOMEM;
		goto out_fail;
	}
	for (nalloc = 0; nalloc < nslots; nalloc++) {
		r = ntfs_mft_alloc(fs, &slot_no[nalloc], &slot_seq[nalloc]);
		if (r != ST_OK)
			goto free_slots;
	}
	/* Instances: base record, then the fresh records from 0. */
	next_inst = ntfs_eino_base_instances(e, e->next_inst);
	for (uint32_t i = 0; i < e->n; i++)
		if (e->a[i].slot > e->nx)
			e->a[i].inst = slot_inst[e->a[i].slot - e->nx - 1]++;
	/* The list in attribute order, and its attribute. */
	al = (uint8_t *)ntfs_zalloc(al_len);
	if (!al) {
		r = ST_NOMEM;
		goto free_slots;
	}
	ntfs_eino_build_al(e, al, slot_no, slot_seq);
	if (al_res) {
		alb = ntfs_ab_res(NTFS_AT_ATTRIBUTE_LIST, NULL, 0, 0, al, al_len,
				  0);
		if (!alb)
			r = ST_NOMEM;
	} else {
		alb = ntfs_eino_al_nonres(e, al, al_len, &r);
	}
	if (r != ST_OK)
		goto free_slots;
	/* The fresh extension records first (allocate before reference):
	 * their $MFT:$BITMAP bits durable before they are written in use. */
	if (nslots) {
		r = ntfs_barrier(fs);
		if (r != ST_OK)
			goto free_slots;
	}
	for (uint32_t s = 0; s < nslots; s++) {
		ntfs_eino_build(e, e->nx + 1 + s, buf, slot_no[s], slot_seq[s],
				fs->new_usa_ofs, 1, 0, slot_inst[s], 0);
		r = ntfs_mft_write(fs, slot_no[s], buf);
		if (r != ST_OK)
			goto free_slots;
	}
	r = ntfs_barrier(fs);
	if (r != ST_OK)
		goto free_slots;
	/* The base record, with the list after the standard information. */
	r = ntfs_eino_add_raw(e, alb, ~0ULL, 0, 1);
	if (r != ST_OK)
		goto free_slots;
	e->a[e->n - 1].slot = 0;
	e->a[e->n - 1].inst = next_inst++;
	alb = NULL;
	ntfs_eino_sort(e);
	ntfs_eino_build(e, 0, buf, e->no, e->seq, e->usa_ofs, e->usn, e->lsn,
			next_inst, 1);
	r = ntfs_mft_write(fs, e->no, buf);
	if (r != ST_OK)
		goto free_slots;
	/* Committed (see above): what the base record no longer references
	 * is freed; failures here leak, they do not undo. */
	r = ntfs_barrier(fs);
	for (uint32_t k = 0; r == ST_OK && k < e->nx; k++)
		if (!e->x[k].keep)
			r = ntfs_mft_free(fs, e->x[k].no);
	if (r == ST_OK && e->al_nonres)
		r = ntfs_rl_free_clusters(fs, &e->al_rl, 0, e->al_rl.end_vcn);
	if (r != ST_OK)
		ntfs_fs_error(fs, e->no,
			      "records or clusters left behind by a commit "
			      "leaked");
	r = ST_OK;
	e->al_nonres = 0;
	e->nx = 0;
out_ok:
	ntfs_eino_release(e, 1);
	goto out;
free_slots:
	for (uint32_t s = 0; s < nalloc; s++)
		ntfs_mft_free(fs, slot_no[s]);
out_fail:
	ntfs_eino_release(e, 0);
out:
	ntfs_free(buf);
	ntfs_free(al);
	ntfs_free(alb);
	ntfs_free(slot_used);
	ntfs_free(slot_no);
	ntfs_free(slot_seq);
	ntfs_free(slot_inst);
	return r;
}

/* ---- small helpers on top of the editable inode ---- */

/* Record 0 of a fragmented $MFT (it has an attribute list): every
 * attribute stays in the record it is in.  What changes: the extent of
 * $DATA that holds the end (its mapping pairs), the sizes in the first extent, and $BITMAP (one extent, resident or not
 * as its stream is).  Everything is prepared and checked before the
 * write, and only a change that is ONE record write is made (ST_NOSPC
 * otherwise: the caller's allocation fails, nothing is written).  A
 * failure after the write latches the mount (ntfs_fs_broken()) and the
 * call reports success, so the caller never gives back clusters that a
 * written record maps. */
static int ntfs_mft_al_commit(ntfs_fs_t *fs, ntfs_eino_t *e,
			      const ntfs_runlist_t *drl, uint64_t alloc,
			      uint64_t dsize, uint64_t dinit)
{
	ntfs_stream_t *bm = fs->st_mftbmp;
	ntfs_runlist_t old;
	long first, last = -1, bi;
	uint8_t *buf = NULL;
	uint32_t n0;
	int r, wrote = 0;

	/* ---- $DATA: the last extent and the sizes ---- */
	r = ntfs_eino_runlist(e, NTFS_AT_DATA, ntfs_unnamed, 0, &old, &first);
	if (r != ST_OK)
		return r;
	ntfs_rl_free(&old);
	for (long i = first; i >= 0;
	     i = ntfs_eino_find(e, NTFS_AT_DATA, ntfs_unnamed, 0,
				(uint32_t)i + 1))
		last = i;
	{
		ntfs_attr_record_t *la = ntfs_ea_hdr(&e->a[last]);
		int64_t low = la->data.non_resident.lowest_vcn;
		uint32_t cap = ntfs_ext_mp_cap(fs, 0), used, mpo;
		uint8_t *mp, *b;
		int64_t stop = 0;
		int same;

		if (low >= drl->end_vcn)
			return ST_NOSPC; /* $MFT never shrinks below an extent */
		mp = (uint8_t *)kalloc(cap);
		if (!mp)
			return ST_NOMEM;
		if (e->a[last].orec != e->no &&
		    drl->end_vcn > la->data.non_resident.highest_vcn + 1) {
			/* Growth while the last extent is in an extension
			 * record: that record (mapping) and the base record
			 * (sizes) would both change -- not one write. */
			kfree(mp);
			return ST_NOSPC;
		}
		used = ntfs_mp_encode(drl, low, drl->end_vcn, mp, cap, &stop);
		if (!used || stop != drl->end_vcn) {
			kfree(mp);
			return ST_NOSPC;
		}
		mpo = la->data.non_resident.mapping_pairs_offset;
		same = la->data.non_resident.highest_vcn == drl->end_vcn - 1 &&
		       mpo <= la->length && used <= la->length - mpo &&
		       kmemcmp((uint8_t *)la + mpo, mp, used) == 0;
		if (same && last == first)
			same = (uint64_t)la->data.non_resident.allocated_size ==
				       alloc &&
			       (uint64_t)la->data.non_resident.data_size == dsize &&
			       (uint64_t)la->data.non_resident
					       .initialized_size == dinit;
		if (!same) {
			b = ntfs_ab_nonres(NTFS_AT_DATA, NULL, 0, la->flags, low,
					   drl->end_vcn - 1, 0, alloc, dsize, dinit,
					   0, mp, used);
			if (!b) {
				kfree(mp);
				return ST_NOMEM;
			}
			ntfs_free(e->a[last].b);
			e->a[last].b = b;
			e->a[last].dirty = 1;
		}
		kfree(mp);
	}
	if (last != first) {
		/* The first extent carries the sizes. */
		ntfs_attr_record_t *fa = ntfs_ea_hdr(&e->a[first]);

		if ((uint64_t)fa->data.non_resident.allocated_size != alloc ||
		    (uint64_t)fa->data.non_resident.data_size != dsize ||
		    (uint64_t)fa->data.non_resident.initialized_size != dinit) {
			fa->data.non_resident.allocated_size = (int64_t)alloc;
			fa->data.non_resident.data_size = (int64_t)dsize;
			fa->data.non_resident.initialized_size = (int64_t)dinit;
			e->a[first].dirty = 1;
		}
	}
	/* ---- $BITMAP: as its stream is now ---- */
	n0 = e->n;
	bi = ntfs_eino_find(e, NTFS_AT_BITMAP, ntfs_unnamed, 0, 0);
	if (bi < 0)
		return ST_IO;
	if (ntfs_eino_find(e, NTFS_AT_BITMAP, ntfs_unnamed, 0,
			   (uint32_t)bi + 1) >= 0)
		return ST_NOSPC; /* several extents: not changed in place */
	{
		ntfs_attr_record_t *ba = ntfs_ea_hdr(&e->a[bi]);
		uint16_t bflags = ba->flags;

		if (bm->flags & NTFS_ST_RESIDENT) {
			if (ba->non_resident ||
			    ba->data.resident.value_length != bm->res_len ||
			    (bm->res_len &&
			     kmemcmp((uint8_t *)ba + ba->data.resident.value_offset,
				     bm->res_data, bm->res_len) != 0))
				r = ntfs_eino_set_res(e, NTFS_AT_BITMAP,
						      ntfs_unnamed, 0, bflags,
						      bm->res_data, bm->res_len);
		} else if (!ba->non_resident ||
			   (uint64_t)ba->data.non_resident.allocated_size !=
				   bm->alloc_size ||
			   (uint64_t)ba->data.non_resident.data_size !=
				   bm->data_size ||
			   (uint64_t)ba->data.non_resident.initialized_size !=
				   bm->init_size ||
			   ba->data.non_resident.highest_vcn !=
				   bm->rl.end_vcn - 1) {
			r = ntfs_eino_set_nonres(e, NTFS_AT_BITMAP, ntfs_unnamed,
						 0, bflags, 0, &bm->rl,
						 bm->alloc_size, bm->data_size,
						 bm->init_size, 0);
		}
		if (r != ST_OK)
			return r;
	}
	/* Nothing may move: one $BITMAP extent replaced by one, in its
	 * record. */
	if (e->n != n0)
		return ST_NOSPC;
	ntfs_eino_sort(e);
	/* ---- layout: every attribute where it was ---- */
	for (uint32_t i = 0; i < e->n; i++) {
		ntfs_eattr_t *x = &e->a[i];

		x->inst = x->oinst;
		if (x->orec == e->no) {
			x->slot = 0;
			continue;
		}
		x->slot = ~0U;
		for (uint32_t k = 0; k < e->nx; k++)
			if (e->x[k].no == x->orec)
				x->slot = k + 1;
		if (x->slot == ~0U)
			return ST_NOSPC; /* a new attribute: no room in place */
	}
	/* The base record keeps its list: a copy joins the set when the
	 * base record is rewritten. */
	{
		int bdirty = 0;

		for (uint32_t i = 0; i < e->n; i++)
			if (e->a[i].slot == 0 && e->a[i].dirty)
				bdirty = 1;
		if (bdirty) {
			uint8_t *tmp = (uint8_t *)kalloc(fs->rec_size);
			const ntfs_attr_record_t *ala = NULL;
			uint8_t *cp;

			if (!tmp)
				return ST_NOMEM;
			r = ntfs_mft_read(fs, NTFS_FILE_MFT, tmp);
			if (r == ST_OK) {
				ala = ntfs_rec_attr(fs, tmp, 0,
						    NTFS_AT_ATTRIBUTE_LIST, 0, 0, 0);
				if (!ala)
					r = ST_IO;
			}
			cp = r == ST_OK ? (uint8_t *)kalloc(ala->length) : NULL;
			if (r == ST_OK && !cp)
				r = ST_NOMEM;
			if (r == ST_OK)
				mm_memcpy(cp, ala, ala->length);
			if (r == ST_OK) {
				uint16_t inst = ala->instance;

				r = ntfs_eino_add_raw(e, cp, e->no, inst, 0);
				if (r != ST_OK) {
					kfree(cp);
				} else {
					e->a[e->n - 1].slot = 0;
					e->a[e->n - 1].inst = inst;
					ntfs_eino_sort(e);
				}
			}
			kfree(tmp);
			if (r != ST_OK)
				return r;
		}
	}
	/* One record write, or none: two records changing (an extent in
	 * an extension record and the sizes in the base record) could not
	 * reach the device as one step, and a power cut between them would
	 * leave the mapping and the sizes of $MFT disagreeing.  Such a
	 * change is refused ($MFT does not grow; the volume stays as it
	 * is). */
	{
		uint32_t nd = 0;

		for (uint32_t k = 0; k <= e->nx; k++)
			for (uint32_t i = 0; i < e->n; i++)
				if (e->a[i].slot == k && e->a[i].dirty) {
					nd++;
					break;
				}
		if (nd > 1)
			return ST_NOSPC;
	}
	/* Room in every record that changes. */
	for (uint32_t k = 0; k <= e->nx; k++) {
		uint32_t used = 0;

		for (uint32_t i = 0; i < e->n; i++)
			if (e->a[i].slot == k)
				used += ntfs_ea_hdr(&e->a[i])->length;
		if (used > ntfs_rec_cap(fs, k ? e->x[k - 1].usa_ofs :
						e->usa_ofs))
			return ST_NOSPC;
	}
	buf = (uint8_t *)kalloc(fs->rec_size);
	if (!buf)
		return ST_NOMEM;
	/* ---- writes: extension records, then the base record ---- */
	for (uint32_t k = 1; k <= e->nx + 1; k++) {
		uint32_t s = k <= e->nx ? k : 0;
		uint64_t rno = s ? e->x[s - 1].no : e->no;
		int dirty = 0;

		for (uint32_t i = 0; i < e->n; i++)
			if (e->a[i].slot == s && e->a[i].dirty)
				dirty = 1;
		if (!dirty)
			continue;
		if (s)
			ntfs_eino_build(e, s, buf, rno, e->x[s - 1].seq,
					e->x[s - 1].usa_ofs, e->x[s - 1].usn,
					e->x[s - 1].lsn, e->x[s - 1].next_inst, 0);
		else
			ntfs_eino_build(e, 0, buf, e->no, e->seq, e->usa_ofs,
					e->usn, e->lsn, e->next_inst, 1);
		r = ntfs_barrier(fs);
		if (r == ST_OK)
			r = ntfs_mft_write(fs, rno, buf);
		if (r != ST_OK)
			break;
		wrote = 1;
	}
	ntfs_free(buf);
	if (r != ST_OK && wrote) {
		ntfs_fs_broken(fs, NTFS_FILE_MFT, "$MFT update interrupted");
		r = ST_OK;
	}
	return r;
}

/* Record 0: $MFT:$DATA gets runlist `drl' with data size `dsize' and
 * initialized size `dinit', $MFT:$BITMAP the state of its stream.  The
 * record is rewritten in place (S25). */
int ntfs_eino_commit_mft(ntfs_fs_t *fs, const ntfs_runlist_t *drl,
			 uint64_t dsize, uint64_t dinit)
{
	ntfs_stream_t *bm = fs->st_mftbmp;
	ntfs_eino_t *e;
	uint64_t alloc = (uint64_t)drl->end_vcn << fs->cluster_bits;
	int r;

	r = ntfs_eino_load(fs, NTFS_MAKE_MREF(NTFS_FILE_MFT, 0), &e);
	if (r != ST_OK)
		return r;
	if (e->had_al) {
		r = ntfs_mft_al_commit(fs, e, drl, alloc, dsize, dinit);
		ntfs_eino_free(e);
		return r;
	}
	r = ntfs_eino_set_nonres(e, NTFS_AT_DATA, ntfs_unnamed, 0, 0, 0, drl,
				 alloc, dsize, dinit, 0);
	if (r == ST_OK) {
		if (bm->flags & NTFS_ST_RESIDENT)
			r = ntfs_eino_set_res(e, NTFS_AT_BITMAP, ntfs_unnamed, 0,
					      0, bm->res_data, bm->res_len);
		else
			r = ntfs_eino_set_nonres(e, NTFS_AT_BITMAP, ntfs_unnamed,
						 0, 0, 0, &bm->rl,
						 bm->alloc_size, bm->data_size,
						 bm->init_size, 0);
	}
	if (r == ST_OK)
		r = ntfs_eino_commit(e);
	ntfs_eino_free(e);
	return r;
}

/* Grow $MFT:$BITMAP to `need' bytes (zeroed bits). */
int ntfs_mftbm_grow(ntfs_fs_t *fs, uint64_t need)
{
	ntfs_stream_t *bm = fs->st_mftbmp;
	int r;

	if (need <= bm->data_size)
		return ST_OK;
	if (bm->flags & NTFS_ST_RESIDENT) {
		uint8_t *nv = (uint8_t *)ntfs_zalloc(need);
		uint8_t *old = bm->res_data;
		uint32_t old_len = bm->res_len;

		if (!nv)
			return ST_NOMEM;
		if (old_len)
			mm_memcpy(nv, old, old_len);
		bm->res_data = nv;
		bm->res_len = (uint32_t)need;
		r = ntfs_eino_commit_mft(fs, &fs->st_mft->rl,
					 fs->st_mft->data_size,
					 fs->st_mft->init_size);
		if (r != ST_OK) {
			bm->res_data = old;
			bm->res_len = old_len;
			kfree(nv);
			return r;
		}
		kfree(old);
		bm->data_size = bm->init_size = bm->alloc_size = need;
		pagecache_invalidate_file(&fs->sb, ntfs_chain(bm));
		return ST_OK;
	} else {
		ntfs_runlist_t nrl, orl;
		ntfs_run_t *runs = NULL;
		uint32_t nruns = 0;
		uint64_t nalloc = (need + fs->cluster_size - 1) &
				  ~((uint64_t)fs->cluster_size - 1);
		uint64_t oalloc = bm->alloc_size, odata = bm->data_size,
			 oinit = bm->init_size;
		uint8_t *z = NULL;

		r = ntfs_rl_copy(&nrl, &bm->rl);
		if (r != ST_OK)
			return r;
		if (nalloc > oalloc) {
			uint64_t cl = (nalloc - oalloc) >> fs->cluster_bits;
			int64_t goal = nrl.count ?
					       nrl.runs[nrl.count - 1].lcn +
						       nrl.runs[nrl.count - 1].len :
					       -1;

			r = ntfs_clusters_alloc(fs, goal, cl,
						NTFS_AL_META | NTFS_AL_MFT,
						&runs, &nruns);
			for (uint32_t i = 0; r == ST_OK && i < nruns; i++)
				r = ntfs_rl_append(&nrl, nrl.end_vcn,
						   runs[i].lcn, runs[i].len);
		} else {
			nalloc = oalloc;
		}
		/* Zero the new bytes on the device [old init, need). */
		if (r == ST_OK) {
			z = (uint8_t *)ntfs_zalloc(need - oinit);
			r = z ? ntfs_rl_pwrite_rmw(fs, &nrl, oinit, z,
						   need - oinit) :
				ST_NOMEM;
			ntfs_free(z);
		}
		if (r == ST_OK)
			r = ntfs_barrier(fs);
		if (r == ST_OK) {
			orl = bm->rl;
			bm->rl = nrl;
			bm->alloc_size = nalloc;
			bm->data_size = bm->init_size = need;
			r = ntfs_eino_commit_mft(fs, &fs->st_mft->rl,
						 fs->st_mft->data_size,
						 fs->st_mft->init_size);
			if (r != ST_OK) {
				bm->rl = orl;
				bm->alloc_size = oalloc;
				bm->data_size = odata;
				bm->init_size = oinit;
			} else {
				ntfs_rl_free(&orl);
				nrl.runs = NULL;
			}
		}
		if (r != ST_OK) {
			for (uint32_t i = 0; i < nruns; i++)
				ntfs_clusters_free(fs, runs[i].lcn,
						   (uint64_t)runs[i].len);
			ntfs_rl_free(&nrl);
		}
		ntfs_free(runs);
		if (r == ST_OK)
			pagecache_invalidate_range(&fs->sb, ntfs_chain(bm),
						   oinit & ~((uint64_t)PAGE_SIZE -
							     1));
		return r;
	}
}
