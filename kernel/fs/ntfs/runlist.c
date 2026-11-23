/* LikeOS - NTFS filesystem driver
 *
 * Runlists, mapping pairs (decoding, editing, encoding).
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
 * S6  Runlists
 *
 * A non-resident attribute's mapping pairs describe its clusters as runs
 * (VCN -> LCN or sparse).  They are decoded once, when the stream is
 * opened, into one sorted, contiguous, coalesced array; every later
 * mapping is a binary search over it.  The decoder validates every run:
 *
 *   - header byte: length bytes 1..8, LCN-delta bytes 0..8, both fully
 *     inside the attribute (the last byte read lies strictly before the
 *     attribute's end);
 *   - run length > 0, VCN accumulation without overflow, LCN
 *     accumulation without overflow, LCN >= 0 and
 *     LCN + length <= clusters of the volume;
 *   - the extent ends exactly at highest_vcn + 1;
 *   - an empty mapping-pairs array only for a zero-length attribute.
 *
 * Array growth is overflow-checked and capped (NTFS_MAX_RUNS).
 * =========================================================================== */

void ntfs_rl_free(ntfs_runlist_t *rl)
{
	ntfs_free(rl->runs);
	rl->runs = NULL;
	rl->count = 0;
	rl->cap = 0;
	rl->end_vcn = 0;
}

/* Append a run, merging it with the last one when it continues it. */
int ntfs_rl_append(ntfs_runlist_t *rl, int64_t vcn, int64_t lcn,
		   int64_t len)
{
	if (len <= 0)
		return ST_IO;
	if (rl->count && rl->end_vcn != vcn)
		return ST_IO; /* not contiguous: caller's bug or corruption */
	if (rl->count) {
		ntfs_run_t *last = &rl->runs[rl->count - 1];

		if ((last->lcn == NTFS_LCN_HOLE && lcn == NTFS_LCN_HOLE) ||
		    (last->lcn >= 0 && lcn >= 0 &&
		     last->lcn + last->len == lcn)) {
			last->len += len;
			rl->end_vcn += len;
			return ST_OK;
		}
	}
	if (rl->count == rl->cap) {
		uint32_t ncap = rl->cap ? rl->cap * 2 : 16;
		uint64_t bytes;
		ntfs_run_t *n;

		if (ncap > NTFS_MAX_RUNS || ncap < rl->cap)
			return ST_NOMEM;
		if (!ntfs_mul_ok(ncap, sizeof(ntfs_run_t), &bytes))
			return ST_NOMEM;
		n = (ntfs_run_t *)kalloc((unsigned long)bytes);
		if (!n)
			return ST_NOMEM;
		if (rl->count)
			mm_memcpy(n, rl->runs, rl->count * sizeof(ntfs_run_t));
		ntfs_free(rl->runs);
		rl->runs = n;
		rl->cap = ncap;
	}
	rl->runs[rl->count].vcn = vcn;
	rl->runs[rl->count].lcn = lcn;
	rl->runs[rl->count].len = len;
	rl->count++;
	rl->end_vcn = vcn + len;
	return ST_OK;
}

/* Index of the run holding `vcn', or -1. */
static long ntfs_rl_find(const ntfs_runlist_t *rl, int64_t vcn)
{
	long lo = 0, hi = (long)rl->count - 1;

	if (!rl->count || vcn < rl->runs[0].vcn || vcn >= rl->end_vcn)
		return -1;
	while (lo <= hi) {
		long mid = lo + (hi - lo) / 2;
		const ntfs_run_t *r = &rl->runs[mid];

		if (vcn < r->vcn)
			hi = mid - 1;
		else if (vcn >= r->vcn + r->len)
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

/* Map `vcn': ST_OK with *lcn (NTFS_LCN_HOLE for a sparse run) and *left
 * (clusters left in the run, counted from vcn); ST_IO when the runlist does
 * not describe it (never a hole). */
int ntfs_rl_map(const ntfs_runlist_t *rl, int64_t vcn, int64_t *lcn,
		int64_t *left)
{
	long i = ntfs_rl_find(rl, vcn);
	const ntfs_run_t *r;

	if (i < 0)
		return ST_IO;
	r = &rl->runs[i];
	*lcn = r->lcn == NTFS_LCN_HOLE ? NTFS_LCN_HOLE :
					 r->lcn + (vcn - r->vcn);
	if (left)
		*left = r->len - (vcn - r->vcn);
	return ST_OK;
}

/* Sign-extended little-endian integer of `n' bytes (1..8). */
static int64_t ntfs_mp_sval(const uint8_t *p, unsigned n)
{
	uint64_t v = 0;

	for (unsigned i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (8 * i);
	if (n < 8 && (p[n - 1] & 0x80))
		v |= ~0ULL << (8 * n);
	return (int64_t)v;
}

/* Decode the mapping pairs of one extent (non-resident attribute record
 * `a', `alen' bytes, already checked by ntfs_attr_valid()) and append its
 * runs to `rl'.  `*lcn_acc' carries nothing between extents: every extent
 * starts its LCN deltas from 0. */
int ntfs_mp_decode(ntfs_fs_t *fs, const ntfs_attr_record_t *a,
		   uint32_t alen, ntfs_runlist_t *rl)
{
	const uint8_t *base = (const uint8_t *)a;
	uint32_t mpo = a->data.non_resident.mapping_pairs_offset;
	int64_t lowest = a->data.non_resident.lowest_vcn;
	int64_t highest = a->data.non_resident.highest_vcn;
	const uint8_t *p = base + mpo;
	const uint8_t *end = base + alen;
	int64_t vcn = lowest;
	int64_t lcn = 0;

	if (mpo > alen || lowest < 0 || highest < lowest - 1)
		return ST_IO;
	if (rl->count && rl->end_vcn != lowest)
		return ST_IO; /* extents must continue each other */
	if (!rl->count && lowest != 0)
		return ST_IO;
	if (p >= end || *p == 0) {
		/* Empty mapping pairs only for an empty attribute. */
		if (lowest == 0 && highest == -1 &&
		    a->data.non_resident.allocated_size == 0 &&
		    a->data.non_resident.data_size == 0 &&
		    a->data.non_resident.initialized_size == 0)
			return ST_OK;
		if (highest == lowest - 1 && lowest > 0)
			return ST_OK; /* an empty continuation extent */
		return ST_IO;
	}
	while (p < end && *p) {
		unsigned lb = *p & 0x0F;
		unsigned cb = (*p >> 4) & 0x0F;
		int64_t len, delta = 0;
		uint64_t nv;

		/* Every byte read lies strictly inside the record. */
		if (lb == 0 || lb > 8 || cb > 8)
			return ST_IO;
		if ((uint64_t)(end - p) <= (uint64_t)lb + cb)
			return ST_IO;
		len = ntfs_mp_sval(p + 1, lb);
		if (len <= 0)
			return ST_IO;
		if (cb)
			delta = ntfs_mp_sval(p + 1 + lb, cb);
		p += 1 + lb + cb;
		/* VCN accumulation never wraps. */
		if (!ntfs_add_ok((uint64_t)vcn, (uint64_t)len, &nv) ||
		    nv > (uint64_t)0x7FFFFFFFFFFFFFFFLL)
			return ST_IO;
		if (cb) {
			/* The LCN accumulator never overflows. */
			if ((delta > 0 && lcn > 0x7FFFFFFFFFFFFFFFLL - delta) ||
			    (delta < 0 && lcn < (-0x7FFFFFFFFFFFFFFFLL - 1) - delta))
				return ST_IO;
			lcn += delta;
			/* Inside the volume. */
			if (lcn < 0 || (uint64_t)lcn >= fs->nr_clusters ||
			    (uint64_t)len > fs->nr_clusters - (uint64_t)lcn)
				return ST_IO;
			if (ntfs_rl_append(rl, vcn, lcn, len) != ST_OK)
				return ST_IO;
		} else {
			if (ntfs_rl_append(rl, vcn, NTFS_LCN_HOLE, len) != ST_OK)
				return ST_IO;
		}
		vcn = (int64_t)nv; /* > 0: a run has a positive length */
		if (vcn - 1 > highest) /* (highest + 1 may overflow) */
			return ST_IO;
	}
	if (p >= end)
		return ST_IO; /* no terminator inside the attribute */
	if (vcn - 1 != highest)
		return ST_IO;
	return ST_OK;
}

/* ===========================================================================
 * S26  Runlist editing and the mapping-pairs encoder
 *
 * In-memory runlists stay sorted, contiguous from VCN 0 and coalesced
 * (S6).  Editing builds a new array and swaps it in only when everything
 * succeeded, so an allocation failure leaves the old runlist untouched
 * (no element pointer is kept across a modification; the
 * caller keeps the old copy for a rollback).
 *
 * The encoder is the exact inverse of ntfs_mp_decode(): one header byte
 * (low nibble: bytes of the run length, high nibble: bytes of the LCN
 * delta, 0 for a sparse run), the length and the delta as little-endian
 * two's complement numbers of the fewest bytes that decode to the same
 * value (a length is positive, so its top byte never has the sign bit),
 * the delta relative to the previous run's LCN of the same extent
 * (starting from 0 in every extent), a terminating 0 byte.
 * =========================================================================== */

/* Copy a runlist. */
int ntfs_rl_copy(ntfs_runlist_t *dst, const ntfs_runlist_t *src)
{
	mm_memset(dst, 0, sizeof(*dst));
	for (uint32_t i = 0; i < src->count; i++) {
		int r = ntfs_rl_append(dst, src->runs[i].vcn, src->runs[i].lcn,
				       src->runs[i].len);

		if (r != ST_OK) {
			ntfs_rl_free(dst);
			return r;
		}
	}
	return ST_OK;
}

/* Clusters of `rl' that are allocated (not sparse) in [v0, v1). */
uint64_t ntfs_rl_allocated(const ntfs_runlist_t *rl, int64_t v0,
			   int64_t v1)
{
	uint64_t n = 0;

	for (uint32_t i = 0; i < rl->count; i++) {
		const ntfs_run_t *x = &rl->runs[i];
		int64_t s = x->vcn > v0 ? x->vcn : v0;
		int64_t e = x->vcn + x->len < v1 ? x->vcn + x->len : v1;

		if (e > s && x->lcn != NTFS_LCN_HOLE)
			n += (uint64_t)(e - s);
	}
	return n;
}

/* Replace [vcn, vcn + len) of `rl' by one run at `lcn' (NTFS_LCN_HOLE for
 * a sparse run).  The range may extend the runlist; a gap between its end
 * and `vcn' becomes sparse.  All or nothing. */
int ntfs_rl_set(ntfs_runlist_t *rl, int64_t vcn, int64_t len,
		int64_t lcn)
{
	ntfs_runlist_t n;
	int64_t end = vcn + len;
	int r = ST_OK;

	if (vcn < 0 || len <= 0)
		return ST_INVALID;
	mm_memset(&n, 0, sizeof(n));
	for (uint32_t i = 0; i < rl->count && r == ST_OK; i++) {
		const ntfs_run_t *x = &rl->runs[i];
		int64_t xe = x->vcn + x->len;

		if (xe <= vcn) {
			r = ntfs_rl_append(&n, x->vcn, x->lcn, x->len);
		} else if (x->vcn < vcn) {
			r = ntfs_rl_append(&n, x->vcn, x->lcn, vcn - x->vcn);
		}
	}
	if (r == ST_OK && n.end_vcn < vcn)
		r = ntfs_rl_append(&n, n.end_vcn, NTFS_LCN_HOLE,
				   vcn - n.end_vcn);
	if (r == ST_OK)
		r = ntfs_rl_append(&n, vcn, lcn, len);
	for (uint32_t i = 0; i < rl->count && r == ST_OK; i++) {
		const ntfs_run_t *x = &rl->runs[i];
		int64_t xe = x->vcn + x->len;

		if (xe <= end)
			continue;
		if (x->vcn >= end)
			r = ntfs_rl_append(&n, x->vcn, x->lcn, x->len);
		else
			r = ntfs_rl_append(&n, end,
					   x->lcn == NTFS_LCN_HOLE ?
						   NTFS_LCN_HOLE :
						   x->lcn + (end - x->vcn),
					   xe - end);
	}
	if (r != ST_OK) {
		ntfs_rl_free(&n);
		return r;
	}
	ntfs_rl_free(rl);
	*rl = n;
	return ST_OK;
}

/* Cut `rl' at `end_vcn' (everything from there on is dropped). */
int ntfs_rl_truncate(ntfs_runlist_t *rl, int64_t end_vcn)
{
	ntfs_runlist_t n;
	int r = ST_OK;

	if (end_vcn >= rl->end_vcn)
		return ST_OK;
	mm_memset(&n, 0, sizeof(n));
	for (uint32_t i = 0; i < rl->count && r == ST_OK; i++) {
		const ntfs_run_t *x = &rl->runs[i];

		if (x->vcn >= end_vcn)
			break;
		r = ntfs_rl_append(&n, x->vcn, x->lcn,
				   x->vcn + x->len > end_vcn ? end_vcn - x->vcn :
							       x->len);
	}
	if (r != ST_OK) {
		ntfs_rl_free(&n);
		return r;
	}
	ntfs_rl_free(rl);
	*rl = n;
	return ST_OK;
}

/* Fewest bytes of a little-endian two's complement number that decodes
 * (sign-extended, ntfs_mp_sval()) to `v'. */
static unsigned ntfs_mp_vbytes(int64_t v)
{
	unsigned n = 1;

	while (n < 8) {
		int64_t lim = (int64_t)1 << (8 * n - 1);

		if (v >= -lim && v < lim)
			break;
		n++;
	}
	return n;
}

/* Encode the runs of `rl' in [v0, v1) as mapping pairs into `buf' (NULL:
 * size only), at most `cap' bytes including the terminator.  *stop gets
 * the first VCN not encoded (v1 when everything fitted); returns the
 * bytes used, or 0 when not even the terminator fits. */
uint32_t ntfs_mp_encode(const ntfs_runlist_t *rl, int64_t v0,
			int64_t v1, uint8_t *buf, uint32_t cap,
			int64_t *stop)
{
	uint32_t used = 0;
	int64_t prev = 0;
	long i = v0 < rl->end_vcn ? ntfs_rl_find(rl, v0) : -1;

	*stop = v0;
	if (cap < 1)
		return 0;
	for (; i >= 0 && (uint32_t)i < rl->count; i++) {
		const ntfs_run_t *x = &rl->runs[i];
		int64_t s = x->vcn > v0 ? x->vcn : v0;
		int64_t e = x->vcn + x->len < v1 ? x->vcn + x->len : v1;
		int64_t len = e - s, lcn, delta = 0;
		unsigned lb, cb = 0;

		if (s >= v1)
			break;
		if (len <= 0)
			continue;
		lb = ntfs_mp_vbytes(len);
		if (x->lcn != NTFS_LCN_HOLE) {
			lcn = x->lcn + (s - x->vcn);
			delta = lcn - prev;
			cb = ntfs_mp_vbytes(delta);
		} else {
			lcn = prev;
		}
		if (used + 1 + lb + cb + 1 > cap)
			break; /* the rest goes into the next extent */
		if (buf) {
			uint8_t *p = buf + used;

			p[0] = (uint8_t)(lb | (cb << 4));
			for (unsigned k = 0; k < lb; k++)
				p[1 + k] = (uint8_t)((uint64_t)len >> (8 * k));
			for (unsigned k = 0; k < cb; k++)
				p[1 + lb + k] =
					(uint8_t)((uint64_t)delta >> (8 * k));
		}
		used += 1 + lb + cb;
		prev = lcn;
		*stop = e;
	}
	if (buf)
		buf[used] = 0;
	used++;
	if (*stop < v0)
		*stop = v0;
	return used;
}
