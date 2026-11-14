/* libgpt -- reading, checking, repairing and writing GUID partition tables.
 *
 * Disk layout (LBA = logical sector):
 *
 *   0                      protective MBR
 *   1                      primary header (my_lba 1, alternate_lba = last)
 *   2 .. 2+E-1             primary entry array (E sectors; 16 KiB normally)
 *   first_usable .. last_usable   the partitions
 *   last-E .. last-1       backup entry array
 *   last                   backup header (my_lba last, alternate_lba 1)
 *
 * Each header carries the CRC-32 of its own first header_size bytes
 * (computed with the CRC field zero) and the CRC-32 of the entry array it
 * points to.  A copy is taken as good only when its signature, size,
 * my_lba, usable range, entry geometry, header CRC and array CRC all check
 * out.  The primary copy is preferred; the backup is found through the
 * primary's alternate_lba, or at the last sector of the disk.
 *
 * Writing always puts the backup at the very end of the disk; a backup
 * found anywhere else (the disk was enlarged, an image was copied to a
 * larger disk) is reported and moved by gpt_repair(), which also widens
 * the usable range to the new end.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpt_internal.h"

int gpt_fail(struct gpt_table *t, int err, const char *fmt, ...)
{
	va_list ap;

	if (err < 0)
		err = -err;
	if (!err)
		err = EINVAL;
	if (t) {
		va_start(ap, fmt);
		vsnprintf(t->errmsg, sizeof(t->errmsg), fmt, ap);
		va_end(ap);
	}
	return -err;
}

/* ---- tables in memory ---------------------------------------------------- */

static int gpt_table_alloc_parts(struct gpt_table *t, uint32_t n)
{
	t->parts = calloc(n, sizeof(*t->parts));
	if (!t->parts)
		return -ENOMEM;
	t->num_entries = n;
	return 0;
}

int gpt_table_new(struct gpt_table *t, uint32_t sector_size,
		  uint64_t nsectors)
{
	uint64_t esec;
	int rc;

	if (!t)
		return -EINVAL;
	memset(t, 0, sizeof(*t));
	if (sector_size < 512 || (sector_size & (sector_size - 1)) != 0)
		return gpt_fail(t, EINVAL, "sector size %u is not supported",
				sector_size);
	esec = gpt_entry_sectors(GPT_DEFAULT_ENTRIES, GPT_ENTRY_SIZE,
				 sector_size);
	/* PMBR + 2 headers + 2 arrays + at least one alignment unit. */
	if (nsectors < 3 + 2 * esec + 2)
		return gpt_fail(t, ENOSPC, "the disk is too small for a GUID "
				"partition table");
	rc = gpt_table_alloc_parts(t, GPT_DEFAULT_ENTRIES);
	if (rc)
		return gpt_fail(t, -rc, "out of memory");
	t->sector_size = sector_size;
	t->nsectors = nsectors;
	t->entry_size = GPT_ENTRY_SIZE;
	t->revision = GPT_REVISION_1_0;
	t->primary_lba = 1;
	t->backup_lba = nsectors - 1;
	t->primary_entries_lba = 2;
	t->backup_entries_lba = nsectors - 1 - esec;
	t->first_usable = 2 + esec;
	t->last_usable = nsectors - 2 - esec;
	rc = gpt_guid_random(&t->disk_guid);
	if (rc) {
		free(t->parts);
		t->parts = NULL;
		return gpt_fail(t, -rc, "no random numbers for the disk GUID");
	}
	t->status.primary_header_ok = 1;
	t->status.primary_entries_ok = 1;
	t->status.backup_header_ok = 1;
	t->status.backup_entries_ok = 1;
	t->status.pmbr_kind = MBR_KIND_PROTECTIVE;
	return 0;
}

int gpt_table_copy(struct gpt_table *dst, const struct gpt_table *src)
{
	if (!dst || !src)
		return -EINVAL;
	*dst = *src;
	dst->parts = NULL;
	if (src->num_entries) {
		dst->parts = calloc(src->num_entries, sizeof(*dst->parts));
		if (!dst->parts)
			return -ENOMEM;
		memcpy(dst->parts, src->parts,
		       src->num_entries * sizeof(*dst->parts));
	}
	return 0;
}

void gpt_table_free(struct gpt_table *t)
{
	if (!t)
		return;
	free(t->parts);
	t->parts = NULL;
	t->num_entries = 0;
}

/* ---- headers -------------------------------------------------------------- */

static void gpt_header_decode(const uint8_t *s, struct gpt_header_raw *h)
{
	h->signature = gpt_le64(s + GH_SIGNATURE);
	h->revision = gpt_le32(s + GH_REVISION);
	h->header_size = gpt_le32(s + GH_HEADER_SIZE);
	h->header_crc32 = gpt_le32(s + GH_HEADER_CRC);
	h->reserved = gpt_le32(s + GH_RESERVED);
	h->my_lba = gpt_le64(s + GH_MY_LBA);
	h->alternate_lba = gpt_le64(s + GH_ALT_LBA);
	h->first_usable_lba = gpt_le64(s + GH_FIRST_USABLE);
	h->last_usable_lba = gpt_le64(s + GH_LAST_USABLE);
	memcpy(h->disk_guid, s + GH_DISK_GUID, 16);
	h->entries_lba = gpt_le64(s + GH_ENTRIES_LBA);
	h->num_entries = gpt_le32(s + GH_NUM_ENTRIES);
	h->entry_size = gpt_le32(s + GH_ENTRY_SIZE);
	h->entries_crc32 = gpt_le32(s + GH_ENTRIES_CRC);
}

/* The CRC of a header sector as it should be (CRC field taken as zero). */
static uint32_t gpt_header_crc(const uint8_t *s, uint32_t header_size)
{
	uint8_t tmp[4096];

	if (header_size > sizeof(tmp))
		header_size = sizeof(tmp);
	memcpy(tmp, s, header_size);
	gpt_put32(tmp + GH_HEADER_CRC, 0);
	return gpt_crc32(0, tmp, header_size);
}

int gpt_read_header(struct gpt_dev *d, uint64_t lba,
		    struct gpt_header_raw *hdr, int *crc_ok)
{
	uint8_t *s;
	int rc;

	if (!d || !hdr)
		return -EINVAL;
	if (crc_ok)
		*crc_ok = 0;
	s = malloc(d->sector_size);
	if (!s)
		return -ENOMEM;
	rc = gpt_dev_read(d, lba, 1, s);
	if (rc) {
		free(s);
		return rc;
	}
	gpt_header_decode(s, hdr);
	if (hdr->signature != GPT_SIGNATURE) {
		free(s);
		return -ENOENT;
	}
	if (crc_ok && hdr->header_size >= GPT_HEADER_SIZE &&
	    hdr->header_size <= d->sector_size)
		*crc_ok = gpt_header_crc(s, hdr->header_size) ==
			  hdr->header_crc32;
	free(s);
	return 0;
}

/* One copy as found on disk. */
struct gpt_copy {
	int present;              /* signature found */
	int header_ok;
	int entries_ok;
	struct gpt_header_raw h;
	uint8_t *array;           /* num_entries * entry_size bytes */
	char problem[96];
};

static void gpt_copy_free(struct gpt_copy *c)
{
	free(c->array);
	c->array = NULL;
}

/* Reads and checks the header at lba (is_primary: the copy at LBA 1). */
static int gpt_load_copy(struct gpt_dev *d, uint64_t lba, int is_primary,
			 struct gpt_copy *c)
{
	uint8_t *s;
	uint64_t esec, n = d->nsectors;
	size_t abytes;
	int rc;

	memset(c, 0, sizeof(*c));
	if (lba == 0 || lba >= n) {
		snprintf(c->problem, sizeof(c->problem), "outside the disk");
		return 0;
	}
	s = malloc(d->sector_size);
	if (!s)
		return -ENOMEM;
	rc = gpt_dev_read(d, lba, 1, s);
	if (rc) {
		free(s);
		snprintf(c->problem, sizeof(c->problem), "cannot be read");
		return 0;
	}
	gpt_header_decode(s, &c->h);
	if (c->h.signature != GPT_SIGNATURE) {
		free(s);
		snprintf(c->problem, sizeof(c->problem), "no GPT signature");
		return 0;
	}
	c->present = 1;
	if (c->h.header_size < GPT_HEADER_SIZE ||
	    c->h.header_size > d->sector_size) {
		snprintf(c->problem, sizeof(c->problem),
			 "header size %u is invalid", c->h.header_size);
		free(s);
		return 0;
	}
	if (gpt_header_crc(s, c->h.header_size) != c->h.header_crc32) {
		snprintf(c->problem, sizeof(c->problem),
			 "header checksum mismatch");
		free(s);
		return 0;
	}
	free(s);
	if (c->h.my_lba != lba) {
		snprintf(c->problem, sizeof(c->problem),
			 "header says it is at LBA %llu, found at %llu",
			 (unsigned long long)c->h.my_lba,
			 (unsigned long long)lba);
		return 0;
	}
	if (c->h.entry_size < 128 || c->h.entry_size % 128 != 0 ||
	    c->h.entry_size > 4096) {
		snprintf(c->problem, sizeof(c->problem),
			 "entry size %u is invalid", c->h.entry_size);
		return 0;
	}
	if (c->h.num_entries == 0 || c->h.num_entries > GPT_MAX_ENTRIES) {
		snprintf(c->problem, sizeof(c->problem),
			 "entry count %u is not supported", c->h.num_entries);
		return 0;
	}
	esec = gpt_entry_sectors(c->h.num_entries, c->h.entry_size,
				 d->sector_size);
	if (c->h.first_usable_lba > c->h.last_usable_lba ||
	    c->h.last_usable_lba >= n) {
		snprintf(c->problem, sizeof(c->problem),
			 "usable range %llu-%llu is invalid",
			 (unsigned long long)c->h.first_usable_lba,
			 (unsigned long long)c->h.last_usable_lba);
		return 0;
	}
	/* Compared without the sum: a wild entries_lba would wrap it. */
	if (c->h.entries_lba < 2 || esec > n || c->h.entries_lba > n - esec) {
		snprintf(c->problem, sizeof(c->problem),
			 "entry array LBA %llu is invalid",
			 (unsigned long long)c->h.entries_lba);
		return 0;
	}
	if (is_primary) {
		if (c->h.entries_lba + esec > c->h.first_usable_lba) {
			snprintf(c->problem, sizeof(c->problem),
				 "entry array overlaps the usable area");
			return 0;
		}
	} else {
		/* A table rebuilt from the backup puts the primary header and
		 * array back at LBA 1 and 2: the usable area must start
		 * after them, as it does in the primary copy. */
		if (c->h.first_usable_lba < 2 + esec) {
			snprintf(c->problem, sizeof(c->problem),
				 "usable range %llu-%llu overlaps the primary "
				 "table",
				 (unsigned long long)c->h.first_usable_lba,
				 (unsigned long long)c->h.last_usable_lba);
			return 0;
		}
		if (c->h.entries_lba <= c->h.last_usable_lba ||
		    c->h.entries_lba + esec > lba) {
			snprintf(c->problem, sizeof(c->problem),
				 "backup entry array is misplaced");
			return 0;
		}
	}
	c->header_ok = 1;
	abytes = (size_t)c->h.num_entries * c->h.entry_size;
	c->array = malloc((size_t)(esec * d->sector_size));
	if (!c->array)
		return -ENOMEM;
	rc = gpt_dev_read(d, c->h.entries_lba, esec, c->array);
	if (rc) {
		snprintf(c->problem, sizeof(c->problem),
			 "entry array cannot be read");
		return 0;
	}
	if (gpt_crc32(0, c->array, abytes) != c->h.entries_crc32) {
		snprintf(c->problem, sizeof(c->problem),
			 "entry array checksum mismatch");
		return 0;
	}
	c->entries_ok = 1;
	return 0;
}

static int gpt_headers_match(const struct gpt_header_raw *a,
			     const struct gpt_header_raw *b)
{
	return a->first_usable_lba == b->first_usable_lba &&
	       a->last_usable_lba == b->last_usable_lba &&
	       memcmp(a->disk_guid, b->disk_guid, 16) == 0 &&
	       a->num_entries == b->num_entries &&
	       a->entry_size == b->entry_size &&
	       a->entries_crc32 == b->entries_crc32;
}

/* Fills t from one good copy. */
static int gpt_table_from_copy(struct gpt_dev *d, struct gpt_table *t,
			       const struct gpt_copy *c)
{
	uint64_t esec = gpt_entry_sectors(c->h.num_entries, c->h.entry_size,
					  d->sector_size);
	int rc;

	rc = gpt_table_alloc_parts(t, c->h.num_entries);
	if (rc)
		return rc;
	t->sector_size = d->sector_size;
	t->nsectors = d->nsectors;
	gpt_guid_from_disk(c->h.disk_guid, &t->disk_guid);
	t->entry_size = c->h.entry_size;
	t->revision = c->h.revision ? c->h.revision : GPT_REVISION_1_0;
	t->first_usable = c->h.first_usable_lba;
	t->last_usable = c->h.last_usable_lba;
	t->primary_lba = 1;
	t->backup_lba = d->nsectors - 1;
	t->backup_entries_lba = d->nsectors - 1 - esec;
	/* The primary array stays where the primary header put it (2 almost
	 * always); a table recovered from the backup gets the usual place. */
	if (c->h.my_lba == 1)
		t->primary_entries_lba = c->h.entries_lba;
	else
		t->primary_entries_lba = 2;
	for (uint32_t i = 0; i < c->h.num_entries; i++)
		gpt_entry_decode(c->array + (size_t)i * c->h.entry_size,
				 &t->parts[i]);
	return 0;
}

int gpt_read(struct gpt_dev *d, struct gpt_table *t)
{
	struct gpt_copy p, b;
	struct mbr_table mbr;
	uint64_t last;
	uint64_t backup_at;
	int rc;

	if (!d || !t)
		return -EINVAL;
	memset(t, 0, sizeof(*t));
	last = d->nsectors - 1;
	if (mbr_read(d, &mbr) == 0)
		t->status.pmbr_kind = mbr.kind;

	rc = gpt_load_copy(d, 1, 1, &p);
	if (rc)
		return rc;
	/* The backup: where the primary says, else the last sector. */
	backup_at = last;
	if (p.header_ok && p.h.alternate_lba > 1 && p.h.alternate_lba <= last)
		backup_at = p.h.alternate_lba;
	rc = gpt_load_copy(d, backup_at, 0, &b);
	if (rc) {
		gpt_copy_free(&p);
		return rc;
	}
	if (!b.header_ok && backup_at != last) {
		struct gpt_copy b2;

		rc = gpt_load_copy(d, last, 0, &b2);
		if (rc) {
			gpt_copy_free(&p);
			gpt_copy_free(&b);
			return rc;
		}
		if (b2.header_ok) {
			gpt_copy_free(&b);
			b = b2;
			backup_at = last;
		} else {
			gpt_copy_free(&b2);
		}
	}

	t->status.primary_header_ok = p.header_ok;
	t->status.primary_entries_ok = p.entries_ok;
	t->status.backup_header_ok = b.header_ok;
	t->status.backup_entries_ok = b.entries_ok;
	snprintf(t->status.primary_problem, sizeof(t->status.primary_problem),
		 "%s", p.problem);
	snprintf(t->status.backup_problem, sizeof(t->status.backup_problem),
		 "%s", b.problem);
	if (b.header_ok && backup_at != last)
		t->status.backup_not_at_end = 1;
	/* The primary points somewhere else than the last sector: whether
	 * the backup was found there, found at the end instead or not at
	 * all, the primary's alternate_lba needs rewriting. */
	if (p.header_ok && p.h.alternate_lba != last)
		t->status.backup_not_at_end = 1;
	if (p.header_ok && p.entries_ok && b.header_ok && b.entries_ok &&
	    !gpt_headers_match(&p.h, &b.h))
		t->status.headers_differ = 1;

	if (p.header_ok && p.entries_ok) {
		rc = gpt_table_from_copy(d, t, &p);
	} else if (b.header_ok && b.entries_ok) {
		t->status.using_backup = 1;
		rc = gpt_table_from_copy(d, t, &b);
	} else if (!p.present && !b.present) {
		rc = -ENOENT;
	} else {
		rc = -EBADMSG;
	}
	gpt_copy_free(&p);
	gpt_copy_free(&b);
	if (rc) {
		struct gpt_status keep = t->status;

		gpt_table_free(t);
		t->status = keep;
		if (rc == -ENOENT)
			snprintf(t->errmsg, sizeof(t->errmsg),
				 "no GUID partition table on the disk");
		else if (rc == -EBADMSG)
			snprintf(t->errmsg, sizeof(t->errmsg),
				 "both copies of the partition table are "
				 "damaged (primary: %.40s; backup: %.40s)",
				 t->status.primary_problem,
				 t->status.backup_problem);
	}
	return rc;
}

/* ---- writing ------------------------------------------------------------- */

static void gpt_header_encode(const struct gpt_table *t, uint8_t *s,
			      uint64_t my_lba, uint64_t alt_lba,
			      uint64_t entries_lba, uint32_t entries_crc)
{
	uint8_t g[16];

	memset(s, 0, t->sector_size);
	gpt_put64(s + GH_SIGNATURE, GPT_SIGNATURE);
	gpt_put32(s + GH_REVISION, t->revision ? t->revision : GPT_REVISION_1_0);
	gpt_put32(s + GH_HEADER_SIZE, GPT_HEADER_SIZE);
	gpt_put32(s + GH_HEADER_CRC, 0);
	gpt_put32(s + GH_RESERVED, 0);
	gpt_put64(s + GH_MY_LBA, my_lba);
	gpt_put64(s + GH_ALT_LBA, alt_lba);
	gpt_put64(s + GH_FIRST_USABLE, t->first_usable);
	gpt_put64(s + GH_LAST_USABLE, t->last_usable);
	gpt_guid_to_disk(&t->disk_guid, g);
	memcpy(s + GH_DISK_GUID, g, 16);
	gpt_put64(s + GH_ENTRIES_LBA, entries_lba);
	gpt_put32(s + GH_NUM_ENTRIES, t->num_entries);
	gpt_put32(s + GH_ENTRY_SIZE, t->entry_size);
	gpt_put32(s + GH_ENTRIES_CRC, entries_crc);
	gpt_put32(s + GH_HEADER_CRC, gpt_crc32(0, s, GPT_HEADER_SIZE));
}

/* Checks that t can be written to d as it is. */
static int gpt_check_writable(struct gpt_dev *d, struct gpt_table *t)
{
	uint64_t esec;

	if (!t->parts || t->num_entries == 0)
		return gpt_fail(t, EINVAL, "the table has no entry array");
	if (t->entry_size < 128 || t->entry_size % 128)
		return gpt_fail(t, EINVAL, "entry size %u is invalid",
				t->entry_size);
	if (t->sector_size != d->sector_size)
		return gpt_fail(t, EINVAL, "the table is for %u-byte sectors, "
				"the disk has %u-byte sectors", t->sector_size,
				d->sector_size);
	esec = gpt_entry_sectors(t->num_entries, t->entry_size,
				 t->sector_size);
	if (d->nsectors < 3 + 2 * esec + 1)
		return gpt_fail(t, ENOSPC, "the disk is too small");
	t->nsectors = d->nsectors;
	t->primary_lba = 1;
	t->backup_lba = d->nsectors - 1;
	t->backup_entries_lba = t->backup_lba - esec;
	if (t->primary_entries_lba < 2)
		t->primary_entries_lba = 2;
	if (t->primary_entries_lba + esec > t->first_usable)
		return gpt_fail(t, EINVAL, "the first usable sector %llu lies "
				"inside the entry array",
				(unsigned long long)t->first_usable);
	if (t->last_usable >= t->backup_entries_lba) {
		/* The disk is smaller than the table says: shrink the usable
		 * area if no partition is in the way. */
		uint64_t new_last = t->backup_entries_lba - 1;

		for (uint32_t i = 0; i < t->num_entries; i++) {
			const struct gpt_part *p = &t->parts[i];

			if (p->used && p->last_lba > new_last)
				return gpt_fail(t, ENOSPC, "partition %u ends "
						"past the end of the disk",
						i + 1);
		}
		t->last_usable = new_last;
	}
	if (t->first_usable > t->last_usable)
		return gpt_fail(t, ENOSPC, "no usable space on the disk");
	for (uint32_t i = 0; i < t->num_entries; i++) {
		const struct gpt_part *p = &t->parts[i];

		if (!p->used)
			continue;
		if (p->first_lba > p->last_lba)
			return gpt_fail(t, EINVAL, "partition %u ends before "
					"it starts", i + 1);
		if (p->first_lba < t->first_usable ||
		    p->last_lba > t->last_usable)
			return gpt_fail(t, ERANGE, "partition %u lies outside "
					"the usable area", i + 1);
		for (uint32_t j = i + 1; j < t->num_entries; j++) {
			const struct gpt_part *q = &t->parts[j];

			if (q->used && p->first_lba <= q->last_lba &&
			    q->first_lba <= p->last_lba)
				return gpt_fail(t, EEXIST, "partitions %u and "
						"%u overlap", i + 1, j + 1);
		}
	}
	return 0;
}

int gpt_write(struct gpt_dev *d, struct gpt_table *t)
{
	uint8_t *array = NULL, *hdr = NULL;
	uint64_t esec;
	size_t abytes;
	uint32_t crc;
	int rc;

	if (!d || !t)
		return -EINVAL;
	if (!d->writable)
		return gpt_fail(t, EBADF, "the disk is open read-only");
	rc = gpt_check_writable(d, t);
	if (rc)
		return rc;
	esec = gpt_entry_sectors(t->num_entries, t->entry_size,
				 t->sector_size);
	abytes = (size_t)t->num_entries * t->entry_size;
	array = calloc(1, (size_t)(esec * t->sector_size));
	hdr = malloc(t->sector_size);
	if (!array || !hdr) {
		free(array);
		free(hdr);
		return gpt_fail(t, ENOMEM, "out of memory");
	}
	for (uint32_t i = 0; i < t->num_entries; i++)
		gpt_entry_encode(&t->parts[i],
				 array + (size_t)i * t->entry_size,
				 t->entry_size);
	crc = gpt_crc32(0, array, abytes);

	/* Backup first: while the primary is being replaced the backup
	 * already describes the new table, so an interruption leaves at
	 * least one good copy of either the old or the new layout. */
	rc = gpt_dev_write(d, t->backup_entries_lba, esec, array);
	if (!rc) {
		gpt_header_encode(t, hdr, t->backup_lba, 1,
				  t->backup_entries_lba, crc);
		rc = gpt_dev_write(d, t->backup_lba, 1, hdr);
	}
	if (!rc)
		rc = gpt_dev_write(d, t->primary_entries_lba, esec, array);
	if (!rc) {
		gpt_header_encode(t, hdr, 1, t->backup_lba,
				  t->primary_entries_lba, crc);
		rc = gpt_dev_write(d, 1, 1, hdr);
	}
	if (!rc)
		rc = mbr_write_protective(d, 1);
	if (!rc)
		rc = gpt_dev_sync(d);
	free(array);
	free(hdr);
	if (rc)
		return gpt_fail(t, -rc, "writing the partition table failed: %s",
				strerror(-rc));
	t->status.primary_header_ok = 1;
	t->status.primary_entries_ok = 1;
	t->status.backup_header_ok = 1;
	t->status.backup_entries_ok = 1;
	t->status.backup_not_at_end = 0;
	t->status.headers_differ = 0;
	t->status.using_backup = 0;
	t->status.pmbr_kind = MBR_KIND_PROTECTIVE;
	t->status.primary_problem[0] = '\0';
	t->status.backup_problem[0] = '\0';
	return 0;
}

int gpt_repair(struct gpt_dev *d, struct gpt_table *t)
{
	uint64_t esec;

	if (!d || !t)
		return -EINVAL;
	if (!t->parts)
		return gpt_fail(t, EINVAL, "no table to repair from");
	esec = gpt_entry_sectors(t->num_entries, t->entry_size,
				 t->sector_size);
	/* The disk grew (or the table came from a smaller disk): the backup
	 * moves to the new end and the usable area follows it. */
	if (d->nsectors > 2 * esec + 3) {
		uint64_t new_last = d->nsectors - 2 - esec;

		if (new_last > t->last_usable)
			t->last_usable = new_last;
	}
	return gpt_write(d, t);
}

int gpt_wipe(struct gpt_dev *d)
{
	struct gpt_header_raw h;
	uint64_t esec_default, last;
	int ok, rc;

	if (!d)
		return -EINVAL;
	last = d->nsectors - 1;
	esec_default = gpt_entry_sectors(GPT_DEFAULT_ENTRIES, GPT_ENTRY_SIZE,
					 d->sector_size);
	/* Where the headers say their arrays are, when they say so. */
	if (gpt_read_header(d, 1, &h, &ok) == 0 && ok &&
	    h.entries_lba < d->nsectors && h.num_entries <= GPT_MAX_ENTRIES &&
	    h.entry_size >= 128 && h.entry_size <= 4096) {
		uint64_t es = gpt_entry_sectors(h.num_entries, h.entry_size,
						d->sector_size);

		if (h.entries_lba + es <= d->nsectors) {
			rc = gpt_dev_zero(d, h.entries_lba * d->sector_size,
					  es * d->sector_size);
			if (rc)
				return rc;
		}
		if (h.alternate_lba > 1 && h.alternate_lba < d->nsectors) {
			struct gpt_header_raw bh;

			/* The same bounds as for the primary: an entry size
			 * from a damaged header would zero far too much. */
			if (gpt_read_header(d, h.alternate_lba, &bh, &ok) == 0 &&
			    ok && bh.entries_lba < d->nsectors &&
			    bh.entry_size >= 128 && bh.entry_size <= 4096) {
				uint64_t bes = gpt_entry_sectors(
					bh.num_entries, bh.entry_size,
					d->sector_size);

				if (bh.entries_lba + bes <= d->nsectors &&
				    bh.num_entries <= GPT_MAX_ENTRIES) {
					rc = gpt_dev_zero(
						d, bh.entries_lba * d->sector_size,
						bes * d->sector_size);
					if (rc)
						return rc;
				}
			}
			rc = gpt_dev_zero(d, h.alternate_lba * d->sector_size,
					  d->sector_size);
			if (rc)
				return rc;
		}
	}
	/* LBA 0, the primary header and the usual array place. */
	rc = gpt_dev_zero(d, 0, (2 + esec_default) * d->sector_size);
	if (rc)
		return rc;
	/* The usual backup array and header at the end. */
	rc = gpt_dev_zero(d, (last - esec_default) * d->sector_size,
			  (esec_default + 1) * d->sector_size);
	if (rc)
		return rc;
	return gpt_dev_sync(d);
}

/* ---- verification ------------------------------------------------------- */

static void gpt_report(gpt_report_fn fn, void *user, int sev, int *errors,
		       const char *fmt, ...) __attribute__((format(printf, 5, 6)));

static void gpt_report(gpt_report_fn fn, void *user, int sev, int *errors,
		       const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (sev == GPT_SEV_ERROR)
		(*errors)++;
	if (fn)
		fn(sev, msg, user);
}

int gpt_verify(struct gpt_dev *d, const struct gpt_table *t,
	       gpt_report_fn report, void *user)
{
	int errors = 0;
	uint64_t align, esec;
	uint32_t phys_lba = 1;

	if (!t || !t->parts)
		return -EINVAL;
	esec = gpt_entry_sectors(t->num_entries, t->entry_size, t->sector_size);
	align = gpt_align_sectors(t);
	if (d && d->phys_sector_size > d->sector_size)
		phys_lba = d->phys_sector_size / d->sector_size;

	/* What is on the disk. */
	if (d) {
		struct gpt_table cur;
		int rc = gpt_read(d, &cur);
		struct mbr_table mbr;

		if (rc == 0 || rc == -EBADMSG) {
			const struct gpt_status *s = &cur.status;

			if (!s->primary_header_ok)
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "primary header: %s",
					   s->primary_problem);
			else if (!s->primary_entries_ok)
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "primary entry array: %s",
					   s->primary_problem);
			if (!s->backup_header_ok)
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "backup header: %s",
					   s->backup_problem);
			else if (!s->backup_entries_ok)
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "backup entry array: %s",
					   s->backup_problem);
			if (s->backup_not_at_end)
				gpt_report(report, user, GPT_SEV_WARNING,
					   &errors, "the backup table is not at "
					   "the end of the disk (the disk "
					   "grew?); repair moves it");
			if (s->headers_differ)
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "the primary and backup tables "
					   "describe different layouts");
			if (s->using_backup)
				gpt_report(report, user, GPT_SEV_INFO, &errors,
					   "the table was read from the backup "
					   "copy");
		} else if (rc == -ENOENT) {
			gpt_report(report, user, GPT_SEV_ERROR, &errors,
				   "no GUID partition table on the disk");
		}
		if (rc == 0)
			gpt_table_free(&cur);
		if (mbr_read(d, &mbr) == 0) {
			switch (mbr.kind) {
			case MBR_KIND_NONE:
			case MBR_KIND_EMPTY:
				gpt_report(report, user, GPT_SEV_WARNING,
					   &errors, "no protective MBR");
				break;
			case MBR_KIND_HYBRID:
				gpt_report(report, user, GPT_SEV_WARNING,
					   &errors, "the MBR is a hybrid one");
				break;
			case MBR_KIND_LEGACY:
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "the MBR holds an ordinary partition "
					   "table beside the GPT");
				break;
			case MBR_KIND_PROTECTIVE: {
				uint64_t want = d->nsectors - 1;

				if (want > 0xffffffffULL)
					want = 0xffffffffULL;
				for (int i = 0; i < mbr.nparts; i++) {
					const struct mbr_part *p = &mbr.parts[i];

					if (p->type != MBR_TYPE_GPT_PROTECTIVE)
						continue;
					if (p->first_lba != 1)
						gpt_report(report, user,
							   GPT_SEV_WARNING,
							   &errors,
							   "the protective MBR "
							   "entry starts at %llu, "
							   "not 1",
							   (unsigned long long)
							   p->first_lba);
					if (p->nsectors != want)
						gpt_report(report, user,
							   GPT_SEV_WARNING,
							   &errors,
							   "the protective MBR "
							   "entry does not cover "
							   "the disk");
				}
				break;
			}
			default:
				break;
			}
		}
	}

	/* The table itself. */
	if (gpt_guid_is_zero(&t->disk_guid))
		gpt_report(report, user, GPT_SEV_WARNING, &errors,
			   "the disk GUID is zero");
	if (t->first_usable < t->primary_entries_lba + esec)
		gpt_report(report, user, GPT_SEV_ERROR, &errors,
			   "the first usable sector %llu lies inside the entry "
			   "array", (unsigned long long)t->first_usable);
	if (t->last_usable >= t->nsectors - 1 - esec)
		gpt_report(report, user, GPT_SEV_ERROR, &errors,
			   "the last usable sector %llu lies inside the backup "
			   "area", (unsigned long long)t->last_usable);
	if (t->first_usable > t->last_usable)
		gpt_report(report, user, GPT_SEV_ERROR, &errors,
			   "the usable area is empty");

	for (uint32_t i = 0; i < t->num_entries; i++) {
		const struct gpt_part *p = &t->parts[i];

		if (!p->used) {
			if (p->first_lba || p->last_lba ||
			    !gpt_guid_is_zero(&p->uuid))
				gpt_report(report, user, GPT_SEV_INFO, &errors,
					   "entry %u has no type but other "
					   "fields set", i + 1);
			continue;
		}
		if (p->first_lba > p->last_lba) {
			gpt_report(report, user, GPT_SEV_ERROR, &errors,
				   "partition %u ends (%llu) before it starts "
				   "(%llu)", i + 1,
				   (unsigned long long)p->last_lba,
				   (unsigned long long)p->first_lba);
			continue;
		}
		if (p->first_lba < t->first_usable ||
		    p->last_lba > t->last_usable)
			gpt_report(report, user, GPT_SEV_ERROR, &errors,
				   "partition %u (%llu-%llu) lies outside the "
				   "usable area %llu-%llu", i + 1,
				   (unsigned long long)p->first_lba,
				   (unsigned long long)p->last_lba,
				   (unsigned long long)t->first_usable,
				   (unsigned long long)t->last_usable);
		if (gpt_guid_is_zero(&p->uuid))
			gpt_report(report, user, GPT_SEV_WARNING, &errors,
				   "partition %u has a zero unique GUID", i + 1);
		if (p->first_lba % phys_lba)
			gpt_report(report, user, GPT_SEV_WARNING, &errors,
				   "partition %u does not start on a physical "
				   "sector boundary", i + 1);
		else if (align && p->first_lba % align)
			gpt_report(report, user, GPT_SEV_INFO, &errors,
				   "partition %u is not aligned to 1 MiB",
				   i + 1);
		for (uint32_t j = i + 1; j < t->num_entries; j++) {
			const struct gpt_part *q = &t->parts[j];

			if (!q->used)
				continue;
			if (p->first_lba <= q->last_lba &&
			    q->first_lba <= p->last_lba)
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "partitions %u and %u overlap", i + 1,
					   j + 1);
			if (!gpt_guid_is_zero(&p->uuid) &&
			    gpt_guid_equal(&p->uuid, &q->uuid))
				gpt_report(report, user, GPT_SEV_ERROR, &errors,
					   "partitions %u and %u have the same "
					   "unique GUID", i + 1, j + 1);
		}
	}
	return errors;
}
