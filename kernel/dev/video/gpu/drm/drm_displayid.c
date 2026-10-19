// LikeOS -- display-manager core: DisplayID blocks.
//
// DisplayID is the successor of the EDID descriptor format, carried as an
// EDID extension block (tag DISPLAYID_EXT): a section header, then data
// blocks.  The iterator here walks the data blocks of every DisplayID
// section of an EDID, checking each section's size and checksum and each
// block's bounds, and remembers the base section's structure version and
// primary use; the EDID code reads tiled display, detailed and formula
// timing, display parameter, vendor and embedded CTA blocks through it.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2021 Intel Corporation

#include "drm_edid_internal.h"

enum {
	QUIRK_IGNORE_CHECKSUM,
};

struct displayid_quirk {
	const struct drm_edid_ident ident;
	uint8_t quirks;
};

/* Sinks whose DisplayID section carries a wrong checksum over otherwise
 * good data. */
static const struct displayid_quirk quirks[] = {
	{
		.ident = DRM_EDID_IDENT_INIT('C', 'S', 'O', 5142, "MNE007ZA1-5"),
		.quirks = BIT(QUIRK_IGNORE_CHECKSUM),
	},
};

static uint8_t get_quirks(const struct drm_edid *drm_edid)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(quirks); i++) {
		if (drm_edid_match(drm_edid, &quirks[i].ident))
			return quirks[i].quirks;
	}

	return 0;
}

/* The section header at `index', NULL when it does not fit. */
static const struct displayid_header *
displayid_get_header(const uint8_t *displayid, int length, int index)
{
	const struct displayid_header *base;

	if ((int)sizeof(*base) > length - index)
		return NULL;

	base = (const struct displayid_header *)&displayid[index];

	return base;
}

static const struct displayid_header *
validate_displayid(const uint8_t *displayid, int length, int idx, bool ignore_checksum)
{
	int i, dispid_length;
	uint8_t csum = 0;
	const struct displayid_header *base;

	base = displayid_get_header(displayid, length, idx);
	if (!base)
		return NULL;

	/* +1 for DispID checksum */
	dispid_length = (int)sizeof(*base) + base->bytes + 1;
	if (dispid_length > length - idx)
		return NULL;

	for (i = 0; i < dispid_length; i++)
		csum = (uint8_t)(csum + displayid[idx + i]);
	if (csum && !ignore_checksum)
		return NULL;

	return base;
}

static const uint8_t *find_next_displayid_extension(struct displayid_iter *iter)
{
	const struct displayid_header *base;
	const uint8_t *displayid;
	bool ignore_checksum = (iter->quirks & BIT(QUIRK_IGNORE_CHECKSUM)) != 0;

	displayid = drm_edid_find_extension(iter->drm_edid, DISPLAYID_EXT, &iter->ext_index);
	if (!displayid)
		return NULL;

	/* EDID extensions block checksum isn't for us */
	iter->length = EDID_LENGTH - 1;
	iter->idx = 1;

	base = validate_displayid(displayid, iter->length, iter->idx, ignore_checksum);
	if (!base)
		return NULL;

	iter->length = iter->idx + (int)sizeof(*base) + base->bytes;

	return displayid;
}

void displayid_iter_edid_begin(const struct drm_edid *drm_edid,
			       struct displayid_iter *iter)
{
	edid_memset(iter, 0, sizeof(*iter));

	iter->drm_edid = drm_edid;
	iter->quirks = get_quirks(drm_edid);
}

static const struct displayid_block *
displayid_iter_block(const struct displayid_iter *iter)
{
	const struct displayid_block *block;

	if (!iter->section)
		return NULL;

	block = (const struct displayid_block *)&iter->section[iter->idx];

	if (iter->idx + (int)sizeof(*block) <= iter->length &&
	    iter->idx + (int)sizeof(*block) + block->num_bytes <= iter->length)
		return block;

	return NULL;
}

const struct displayid_block *__displayid_iter_next(struct displayid_iter *iter)
{
	const struct displayid_block *block;

	if (!iter->drm_edid)
		return NULL;

	if (iter->section) {
		/* current block should always be valid */
		block = displayid_iter_block(iter);
		if (!block) {
			iter->section = NULL;
			iter->drm_edid = NULL;
			return NULL;
		}

		/* next block in section */
		iter->idx += (int)sizeof(*block) + block->num_bytes;

		block = displayid_iter_block(iter);
		if (block)
			return block;
	}

	for (;;) {
		/* The first section we encounter is the base section */
		bool base_section = !iter->section;

		iter->section = find_next_displayid_extension(iter);
		if (!iter->section) {
			iter->drm_edid = NULL;
			return NULL;
		}

		/* Save the structure version and primary use case. */
		if (base_section) {
			const struct displayid_header *base;

			base = displayid_get_header(iter->section, iter->length,
						    iter->idx);
			if (base) {
				iter->version = base->rev;
				iter->primary_use = base->prod_id;
			}
		}

		iter->idx += (int)sizeof(struct displayid_header);

		block = displayid_iter_block(iter);
		if (block)
			return block;
	}
}

void displayid_iter_end(struct displayid_iter *iter)
{
	edid_memset(iter, 0, sizeof(*iter));
}

uint8_t displayid_version(const struct displayid_iter *iter)
{
	return iter->version;
}

uint8_t displayid_primary_use(const struct displayid_iter *iter)
{
	return iter->primary_use;
}
