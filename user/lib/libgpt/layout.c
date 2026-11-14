/* libgpt -- partitions in a table: entries, names, free space, edits.
 *
 * All of this works on the table in memory; nothing reaches the disk before
 * gpt_write().  Every edit checks that the result is a valid layout (inside
 * the usable area, no overlaps, first <= last) and says why when it is not
 * (gpt_table.errmsg).
 *
 * New partitions start on a 1 MiB boundary, the alignment every current
 * partitioning tool uses: it is a multiple of every physical sector size
 * and erase-block size in use, so no partition straddles one.
 *
 * Names are stored as 36 UTF-16LE code units; they are offered as UTF-8.
 * Characters outside the basic multilingual plane take two units
 * (a surrogate pair).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "gpt_internal.h"

/* ---- entries ---------------------------------------------------------- */

void gpt_entry_decode(const uint8_t *raw, struct gpt_part *p)
{
	memset(p, 0, sizeof(*p));
	gpt_guid_from_disk(raw + GE_TYPE, &p->type);
	gpt_guid_from_disk(raw + GE_UUID, &p->uuid);
	p->first_lba = gpt_le64(raw + GE_FIRST);
	p->last_lba = gpt_le64(raw + GE_LAST);
	p->attributes = gpt_le64(raw + GE_ATTRS);
	for (int i = 0; i < GPT_NAME_UNITS; i++)
		p->name_raw[i] = gpt_le16(raw + GE_NAME + 2 * i);
	p->used = !gpt_guid_is_zero(&p->type);
	gpt_name_to_utf8(p->name_raw, GPT_NAME_UNITS, p->name, sizeof(p->name));
}

void gpt_entry_encode(const struct gpt_part *p, uint8_t *raw,
		      uint32_t entry_size)
{
	uint8_t g[16];

	memset(raw, 0, entry_size);
	if (!p->used)
		return;
	gpt_guid_to_disk(&p->type, g);
	memcpy(raw + GE_TYPE, g, 16);
	gpt_guid_to_disk(&p->uuid, g);
	memcpy(raw + GE_UUID, g, 16);
	gpt_put64(raw + GE_FIRST, p->first_lba);
	gpt_put64(raw + GE_LAST, p->last_lba);
	gpt_put64(raw + GE_ATTRS, p->attributes);
	for (int i = 0; i < GPT_NAME_UNITS; i++)
		gpt_put16(raw + GE_NAME + 2 * i, p->name_raw[i]);
}

/* ---- names ------------------------------------------------------------- */

/* Appends code point cp as UTF-8 if it fits (keeping room for the
 * terminator); returns the new length or -1 when it does not fit. */
static int gpt_utf8_put(char *out, size_t outlen, size_t o, uint32_t cp)
{
	uint8_t b[4];
	size_t n;

	if (cp < 0x80) {
		b[0] = (uint8_t)cp;
		n = 1;
	} else if (cp < 0x800) {
		b[0] = (uint8_t)(0xc0 | (cp >> 6));
		b[1] = (uint8_t)(0x80 | (cp & 0x3f));
		n = 2;
	} else if (cp < 0x10000) {
		b[0] = (uint8_t)(0xe0 | (cp >> 12));
		b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
		b[2] = (uint8_t)(0x80 | (cp & 0x3f));
		n = 3;
	} else {
		b[0] = (uint8_t)(0xf0 | (cp >> 18));
		b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
		b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
		b[3] = (uint8_t)(0x80 | (cp & 0x3f));
		n = 4;
	}
	if (o + n + 1 > outlen)
		return -1;
	memcpy(out + o, b, n);
	return (int)(o + n);
}

int gpt_name_to_utf8(const uint16_t *name, size_t units, char *out,
		     size_t outlen)
{
	size_t o = 0;

	if (!out || outlen == 0)
		return -EINVAL;
	out[0] = '\0';
	if (!name)
		return 0;
	for (size_t i = 0; i < units; i++) {
		uint32_t cp = name[i];
		int r;

		if (cp == 0)
			break;
		if (cp >= 0xd800 && cp <= 0xdbff) {
			/* A high surrogate: it needs a low one after it. */
			if (i + 1 < units && name[i + 1] >= 0xdc00 &&
			    name[i + 1] <= 0xdfff) {
				cp = 0x10000 + ((cp - 0xd800) << 10) +
				     (uint32_t)(name[i + 1] - 0xdc00);
				i++;
			} else {
				cp = 0xfffd;
			}
		} else if (cp >= 0xdc00 && cp <= 0xdfff) {
			cp = 0xfffd; /* a lone low surrogate */
		}
		r = gpt_utf8_put(out, outlen, o, cp);
		if (r < 0)
			break; /* truncated at a character boundary */
		o = (size_t)r;
	}
	out[o] = '\0';
	return (int)o;
}

/* Decodes one UTF-8 sequence; returns its length, or -1 when it is not
 * well formed (overlong forms, surrogates and values past U+10FFFF are
 * refused). */
static int gpt_utf8_get(const unsigned char *s, uint32_t *cp)
{
	uint32_t c = s[0];
	int n;

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xc2 && c <= 0xdf) {
		n = 2;
		c &= 0x1f;
	} else if (c >= 0xe0 && c <= 0xef) {
		n = 3;
		c &= 0x0f;
	} else if (c >= 0xf0 && c <= 0xf4) {
		n = 4;
		c &= 0x07;
	} else {
		return -1;
	}
	for (int i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return -1;
		c = (c << 6) | (s[i] & 0x3f);
	}
	if ((n == 3 && c < 0x800) || (n == 4 && c < 0x10000) || c > 0x10ffff ||
	    (c >= 0xd800 && c <= 0xdfff))
		return -1;
	*cp = c;
	return n;
}

int gpt_name_from_utf8(const char *utf8, uint16_t name[GPT_NAME_UNITS])
{
	const unsigned char *s = (const unsigned char *)(utf8 ? utf8 : "");
	uint16_t tmp[GPT_NAME_UNITS];
	int u = 0;

	memset(tmp, 0, sizeof(tmp));
	while (*s) {
		uint32_t cp;
		int n = gpt_utf8_get(s, &cp);

		if (n < 0)
			return -EILSEQ;
		s += n;
		if (cp >= 0x10000) {
			if (u + 2 > GPT_NAME_UNITS)
				return -ENAMETOOLONG;
			cp -= 0x10000;
			tmp[u++] = (uint16_t)(0xd800 + (cp >> 10));
			tmp[u++] = (uint16_t)(0xdc00 + (cp & 0x3ff));
		} else {
			if (u + 1 > GPT_NAME_UNITS)
				return -ENAMETOOLONG;
			tmp[u++] = (uint16_t)cp;
		}
	}
	memcpy(name, tmp, sizeof(tmp));
	return u;
}

/* ---- alignment and lookups ------------------------------------------- */

uint64_t gpt_align_sectors(const struct gpt_table *t)
{
	uint64_t a;

	if (!t || t->sector_size == 0)
		return 1;
	a = GPT_ALIGN_BYTES / t->sector_size;
	return a ? a : 1;
}

uint64_t gpt_align_up(uint64_t lba, uint64_t align)
{
	if (align <= 1)
		return lba;
	if (lba % align == 0)
		return lba;
	if (lba > UINT64_MAX - align)
		return UINT64_MAX;
	return (lba / align + 1) * align;
}

uint64_t gpt_align_down(uint64_t lba, uint64_t align)
{
	if (align <= 1)
		return lba;
	return lba / align * align;
}

static int gpt_partno_ok(const struct gpt_table *t, int partno)
{
	return t && t->parts && partno >= 1 &&
	       (uint32_t)partno <= t->num_entries;
}

const struct gpt_part *gpt_part_get(const struct gpt_table *t, int partno)
{
	if (!gpt_partno_ok(t, partno) || !t->parts[partno - 1].used)
		return NULL;
	return &t->parts[partno - 1];
}

int gpt_part_count(const struct gpt_table *t)
{
	int n = 0;

	if (!t || !t->parts)
		return 0;
	for (uint32_t i = 0; i < t->num_entries; i++)
		if (t->parts[i].used)
			n++;
	return n;
}

int gpt_part_first_free_slot(const struct gpt_table *t)
{
	if (!t || !t->parts)
		return -EINVAL;
	for (uint32_t i = 0; i < t->num_entries; i++)
		if (!t->parts[i].used)
			return (int)i + 1;
	return -ENOSPC;
}

int gpt_overlap(const struct gpt_table *t, uint64_t first, uint64_t last,
		int ignore)
{
	if (!t || !t->parts)
		return 0;
	for (uint32_t i = 0; i < t->num_entries; i++) {
		const struct gpt_part *p = &t->parts[i];

		if (!p->used || (int)i + 1 == ignore)
			continue;
		if (first <= p->last_lba && p->first_lba <= last)
			return (int)i + 1;
	}
	return 0;
}

int gpt_part_order(const struct gpt_table *t, int *out, int max)
{
	int n = 0;

	if (!t || !t->parts || !out || max < 0)
		return -EINVAL;
	for (uint32_t i = 0; i < t->num_entries; i++) {
		int k;

		if (!t->parts[i].used)
			continue;
		if (n >= max)
			return -ENOSPC;
		/* Insertion sort by first LBA, then by number. */
		k = n;
		while (k > 0 &&
		       t->parts[out[k - 1] - 1].first_lba >
			       t->parts[i].first_lba) {
			out[k] = out[k - 1];
			k--;
		}
		out[k] = (int)i + 1;
		n++;
	}
	return n;
}

/* ---- free space --------------------------------------------------------- */

int gpt_free_regions(const struct gpt_table *t, struct gpt_free *out,
		     int max, int aligned)
{
	int order[GPT_MAX_ENTRIES];
	int n, count = 0;
	uint64_t cursor, align;

	if (!t || !t->parts || (!out && max > 0) || max < 0)
		return -EINVAL;
	if (t->first_usable > t->last_usable)
		return 0;
	n = gpt_part_order(t, order, GPT_MAX_ENTRIES);
	if (n < 0)
		return n;
	align = aligned ? gpt_align_sectors(t) : 1;
	cursor = t->first_usable;
	for (int k = 0; k <= n; k++) {
		uint64_t gap_first = cursor, gap_last;
		int have_gap;

		if (k < n) {
			const struct gpt_part *p = &t->parts[order[k] - 1];

			/* A partition outside the usable area (a damaged
			 * table) still blocks what it covers. */
			have_gap = p->first_lba > cursor;
			gap_last = have_gap ? p->first_lba - 1 : 0;
			/* Saturating: a damaged last_lba of all ones must not
			 * wrap the cursor back to sector 0. */
			if (p->last_lba >= cursor)
				cursor = p->last_lba == UINT64_MAX ? UINT64_MAX :
					 p->last_lba + 1;
			if (gap_last > t->last_usable)
				gap_last = t->last_usable;
		} else {
			have_gap = cursor <= t->last_usable;
			gap_last = t->last_usable;
		}
		if (!have_gap || gap_first > gap_last)
			continue;
		if (aligned) {
			gap_first = gpt_align_up(gap_first, align);
			if (gap_first > gap_last ||
			    gap_last - gap_first + 1 < align)
				continue;
		}
		if (count < max) {
			out[count].first_lba = gap_first;
			out[count].last_lba = gap_last;
		}
		count++;
		if (cursor > t->last_usable)
			break;
	}
	return count < max ? count : max;
}

/* ---- edits --------------------------------------------------------------- */

int gpt_part_add(struct gpt_table *t, const struct gpt_part_spec *spec,
		 int *partno_out)
{
	struct gpt_free regions[GPT_MAX_ENTRIES + 2];
	struct gpt_part np;
	uint64_t first = 0, last = 0;
	int partno, nreg, found = 0, rc;

	if (!t || !t->parts || !spec)
		return -EINVAL;
	t->errmsg[0] = '\0';
	if (spec->partno == 0) {
		partno = gpt_part_first_free_slot(t);
		if (partno < 0)
			return gpt_fail(t, ENOSPC, "all %u partition entries are "
					"in use", t->num_entries);
	} else {
		partno = spec->partno;
		if (!gpt_partno_ok(t, partno))
			return gpt_fail(t, EINVAL, "partition number %d is out of "
					"range (1-%u)", partno, t->num_entries);
		if (t->parts[partno - 1].used)
			return gpt_fail(t, EEXIST, "partition %d already exists",
					partno);
	}

	if (spec->first_lba == 0) {
		/* The first aligned region that holds the size.  Only a disk
		 * too small to hold two alignment units (a tiny image) is
		 * partitioned without alignment; on anything larger the
		 * slivers before the first boundary stay unused. */
		int passes = (t->last_usable - t->first_usable + 1 <
			      2 * gpt_align_sectors(t)) ? 2 : 1;

		for (int pass = 0; pass < passes && !found; pass++) {
			nreg = gpt_free_regions(t, regions,
						(int)(sizeof(regions) /
						      sizeof(regions[0])),
						pass == 0);
			if (nreg < 0)
				return gpt_fail(t, -nreg, "cannot list free space");
			for (int i = 0; i < nreg; i++) {
				uint64_t avail = regions[i].last_lba -
						 regions[i].first_lba + 1;

				if (spec->size_lba && avail < spec->size_lba)
					continue;
				first = regions[i].first_lba;
				last = spec->size_lba ?
					first + spec->size_lba - 1 :
					regions[i].last_lba;
				found = 1;
				break;
			}
		}
		if (!found)
			return gpt_fail(t, ENOSPC, spec->size_lba ?
					"no free region is large enough" :
					"the disk has no free space");
	} else {
		first = spec->first_lba;
		if (first < t->first_usable || first > t->last_usable)
			return gpt_fail(t, ERANGE, "sector %llu lies outside the "
					"usable area %llu-%llu",
					(unsigned long long)first,
					(unsigned long long)t->first_usable,
					(unsigned long long)t->last_usable);
		nreg = gpt_free_regions(t, regions,
					(int)(sizeof(regions) / sizeof(regions[0])),
					0);
		if (nreg < 0)
			return gpt_fail(t, -nreg, "cannot list free space");
		for (int i = 0; i < nreg; i++) {
			if (first < regions[i].first_lba ||
			    first > regions[i].last_lba)
				continue;
			if (spec->size_lba) {
				if (spec->size_lba - 1 >
				    regions[i].last_lba - first)
					return gpt_fail(t, ENOSPC, "the free "
							"region at sector %llu "
							"is too small",
							(unsigned long long)first);
				last = first + spec->size_lba - 1;
			} else {
				last = regions[i].last_lba;
			}
			found = 1;
			break;
		}
		if (!found) {
			int other = gpt_overlap(t, first, first, 0);

			return gpt_fail(t, EEXIST, "sector %llu is inside "
					"partition %d",
					(unsigned long long)first, other);
		}
	}
	if (last < first || last > t->last_usable)
		return gpt_fail(t, ERANGE, "the partition does not fit");
	if (gpt_overlap(t, first, last, 0))
		return gpt_fail(t, EEXIST, "the partition would overlap "
				"partition %d", gpt_overlap(t, first, last, 0));

	memset(&np, 0, sizeof(np));
	np.used = 1;
	np.first_lba = first;
	np.last_lba = last;
	np.attributes = spec->attributes;
	if (gpt_guid_is_zero(&spec->type)) {
		rc = gpt_guid_parse(GPT_TYPE_GENERIC_DATA, &np.type);
		if (rc)
			return gpt_fail(t, -rc, "internal: bad type GUID");
	} else {
		np.type = spec->type;
	}
	if (gpt_guid_is_zero(&spec->uuid)) {
		rc = gpt_guid_random(&np.uuid);
		if (rc)
			return gpt_fail(t, -rc, "no random numbers for the "
					"partition GUID");
	} else {
		np.uuid = spec->uuid;
		for (uint32_t i = 0; i < t->num_entries; i++)
			if (t->parts[i].used &&
			    gpt_guid_equal(&t->parts[i].uuid, &np.uuid))
				return gpt_fail(t, EEXIST, "partition %u already "
						"has that unique GUID", i + 1);
	}
	if (spec->name) {
		rc = gpt_name_from_utf8(spec->name, np.name_raw);
		if (rc < 0)
			return gpt_fail(t, -rc, rc == -ENAMETOOLONG ?
					"the name is longer than 36 UTF-16 "
					"units" : "the name is not valid UTF-8");
		gpt_name_to_utf8(np.name_raw, GPT_NAME_UNITS, np.name,
				 sizeof(np.name));
	}
	t->parts[partno - 1] = np;
	if (partno_out)
		*partno_out = partno;
	return 0;
}

int gpt_part_delete(struct gpt_table *t, int partno)
{
	if (!t || !t->parts)
		return -EINVAL;
	t->errmsg[0] = '\0';
	if (!gpt_partno_ok(t, partno))
		return gpt_fail(t, EINVAL, "partition number %d is out of range",
				partno);
	if (!t->parts[partno - 1].used)
		return gpt_fail(t, ENOENT, "partition %d does not exist", partno);
	memset(&t->parts[partno - 1], 0, sizeof(t->parts[partno - 1]));
	return 0;
}

/* The last sector a partition may end on without moving its start. */
static uint64_t gpt_max_last(const struct gpt_table *t, int partno)
{
	const struct gpt_part *p = &t->parts[partno - 1];
	uint64_t limit = t->last_usable;

	for (uint32_t i = 0; i < t->num_entries; i++) {
		const struct gpt_part *q = &t->parts[i];

		if (!q->used || (int)i + 1 == partno)
			continue;
		if (q->first_lba > p->first_lba && q->first_lba - 1 < limit)
			limit = q->first_lba - 1;
	}
	return limit;
}

uint64_t gpt_part_max_size(const struct gpt_table *t, int partno)
{
	const struct gpt_part *p = gpt_part_get(t, partno);
	uint64_t last;

	if (!p)
		return 0;
	last = gpt_max_last(t, partno);
	if (last < p->first_lba)
		return 0;
	return last - p->first_lba + 1;
}

int gpt_part_resize(struct gpt_table *t, int partno, uint64_t new_size_lba)
{
	struct gpt_part *p;
	uint64_t max_last, new_last;

	if (!t || !t->parts)
		return -EINVAL;
	t->errmsg[0] = '\0';
	if (!gpt_partno_ok(t, partno) || !t->parts[partno - 1].used)
		return gpt_fail(t, ENOENT, "partition %d does not exist", partno);
	p = &t->parts[partno - 1];
	max_last = gpt_max_last(t, partno);
	if (max_last < p->first_lba)
		return gpt_fail(t, ERANGE, "partition %d starts outside the "
				"usable area", partno);
	if (new_size_lba == 0) {
		new_last = max_last;
	} else {
		if (new_size_lba - 1 > max_last - p->first_lba)
			return gpt_fail(t, ENOSPC, "partition %d can grow to at "
					"most %llu sectors", partno,
					(unsigned long long)(max_last -
							     p->first_lba + 1));
		new_last = p->first_lba + new_size_lba - 1;
	}
	p->last_lba = new_last;
	return 0;
}

int gpt_part_set_name(struct gpt_table *t, int partno, const char *utf8)
{
	uint16_t raw[GPT_NAME_UNITS];
	int rc;

	if (!t || !t->parts)
		return -EINVAL;
	t->errmsg[0] = '\0';
	if (!gpt_part_get(t, partno))
		return gpt_fail(t, ENOENT, "partition %d does not exist", partno);
	rc = gpt_name_from_utf8(utf8, raw);
	if (rc < 0)
		return gpt_fail(t, -rc, rc == -ENAMETOOLONG ?
				"the name is longer than 36 UTF-16 units" :
				"the name is not valid UTF-8");
	memcpy(t->parts[partno - 1].name_raw, raw, sizeof(raw));
	gpt_name_to_utf8(raw, GPT_NAME_UNITS, t->parts[partno - 1].name,
			 sizeof(t->parts[partno - 1].name));
	return 0;
}

int gpt_part_set_type(struct gpt_table *t, int partno,
		      const struct gpt_guid *type)
{
	if (!t || !t->parts || !type)
		return -EINVAL;
	t->errmsg[0] = '\0';
	if (!gpt_part_get(t, partno))
		return gpt_fail(t, ENOENT, "partition %d does not exist", partno);
	if (gpt_guid_is_zero(type))
		return gpt_fail(t, EINVAL, "the zero GUID marks an unused entry; "
				"delete the partition instead");
	t->parts[partno - 1].type = *type;
	return 0;
}

int gpt_part_set_uuid(struct gpt_table *t, int partno,
		      const struct gpt_guid *uuid)
{
	struct gpt_guid g;
	int rc;

	if (!t || !t->parts)
		return -EINVAL;
	t->errmsg[0] = '\0';
	if (!gpt_part_get(t, partno))
		return gpt_fail(t, ENOENT, "partition %d does not exist", partno);
	if (!uuid || gpt_guid_is_zero(uuid)) {
		rc = gpt_guid_random(&g);
		if (rc)
			return gpt_fail(t, -rc, "no random numbers");
	} else {
		g = *uuid;
	}
	for (uint32_t i = 0; i < t->num_entries; i++)
		if (t->parts[i].used && (int)i + 1 != partno &&
		    gpt_guid_equal(&t->parts[i].uuid, &g))
			return gpt_fail(t, EEXIST, "partition %u already has "
					"that unique GUID", i + 1);
	t->parts[partno - 1].uuid = g;
	return 0;
}

int gpt_part_set_attributes(struct gpt_table *t, int partno, uint64_t attrs)
{
	if (!t || !t->parts)
		return -EINVAL;
	t->errmsg[0] = '\0';
	if (!gpt_part_get(t, partno))
		return gpt_fail(t, ENOENT, "partition %d does not exist", partno);
	t->parts[partno - 1].attributes = attrs;
	return 0;
}
