// LikeOS -- display-manager core: EDID blocks, containers and iterators.
//
// Checking EDID blocks (header, checksum, version; repair of a header
// that is mostly right), the HDMI forum override of the block count,
// dropping extension blocks that fail, the struct drm_edid container,
// matching a sink against the quirk list, walking the 18-byte descriptors
// of the base block and the CTA and VTB extensions, walking the CTA data
// blocks of CTA extensions and of DisplayID, and the monitor name.  The
// mode and sink parsers (drm_edid_parse.c), the ELD builder (drm_eld.c)
// and the DDC reader (drm_edid.c) build on these.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Luc Verhaegen's, Intel's, Red Hat's and Dennis Munsie's code: MIT
// Portions Copyright (C) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2010 Red Hat, Inc.
// Portions Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>

#include "drm_edid_internal.h"

/*** blocks ***/

static const uint8_t edid_header[] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

int edid_extension_block_count(const struct edid *edid)
{
	return edid->extensions;
}

int edid_block_count(const struct edid *edid)
{
	return edid_extension_block_count(edid) + 1;
}

int edid_size_by_blocks(int num_blocks)
{
	return num_blocks * EDID_LENGTH;
}

int edid_size(const struct edid *edid)
{
	return edid_size_by_blocks(edid_block_count(edid));
}

const void *edid_block_data(const struct edid *edid, int index)
{
	return edid + index;
}

static const void *edid_extension_block_data(const struct edid *edid, int index)
{
	return edid_block_data(edid, index + 1);
}

/* EDID block count indicated in EDID, may exceed allocated size */
int __drm_edid_block_count(const struct drm_edid *drm_edid)
{
	int num_blocks;

	/* Starting point */
	num_blocks = edid_block_count(drm_edid->edid);

	/* HF-EEODB override */
	if (drm_edid->size >= (size_t)edid_size_by_blocks(2)) {
		int eeodb;

		/*
		 * Note: HF-EEODB may specify a smaller extension count than the
		 * regular one. Unlike in buffer allocation, here we can use it.
		 */
		eeodb = edid_hfeeodb_block_count(drm_edid->edid);
		if (eeodb)
			num_blocks = eeodb;
	}

	return num_blocks;
}

/* EDID block count, limited by allocated size */
int drm_edid_block_count(const struct drm_edid *drm_edid)
{
	/* Limit by allocated size */
	return min(__drm_edid_block_count(drm_edid),
		   (int)drm_edid->size / EDID_LENGTH);
}

/* EDID extension block count, limited by allocated size */
int drm_edid_extension_block_count(const struct drm_edid *drm_edid)
{
	return drm_edid_block_count(drm_edid) - 1;
}

const void *drm_edid_block_data(const struct drm_edid *drm_edid, int index)
{
	return edid_block_data(drm_edid->edid, index);
}

const void *drm_edid_extension_block_data(const struct drm_edid *drm_edid, int index)
{
	return edid_extension_block_data(drm_edid->edid, index);
}

/*
 * A container for an EDID whose size is trusted from its own extension
 * count -- for interfaces that are handed a bare struct edid.
 */
const struct drm_edid *drm_edid_legacy_init(struct drm_edid *drm_edid,
					    const struct edid *edid)
{
	if (!edid)
		return NULL;

	edid_memset(drm_edid, 0, sizeof(*drm_edid));

	drm_edid->edid = edid;
	drm_edid->size = (size_t)edid_size(edid);

	return drm_edid;
}

/*
 * EDID base and extension block iterator.
 *
 * struct drm_edid_iter iter;
 * const u8 *block;
 *
 * drm_edid_iter_begin(drm_edid, &iter);
 * drm_edid_iter_for_each(block, &iter) {
 *         // do stuff with block
 * }
 * drm_edid_iter_end(&iter);
 */
void drm_edid_iter_begin(const struct drm_edid *drm_edid, struct drm_edid_iter *iter)
{
	edid_memset(iter, 0, sizeof(*iter));

	iter->drm_edid = drm_edid;
}

const void *__drm_edid_iter_next(struct drm_edid_iter *iter)
{
	const void *block = NULL;

	if (!iter->drm_edid)
		return NULL;

	if (iter->index < drm_edid_block_count(iter->drm_edid))
		block = drm_edid_block_data(iter->drm_edid, iter->index++);

	return block;
}

void drm_edid_iter_end(struct drm_edid_iter *iter)
{
	edid_memset(iter, 0, sizeof(*iter));
}

void edid_header_fix(void *edid)
{
	edid_memcpy(edid, edid_header, sizeof(edid_header));
}

int drm_edid_header_is_valid(const void *_edid)
{
	const struct edid *edid = _edid;
	int score = 0;
	unsigned int i;

	for (i = 0; i < sizeof(edid_header); i++) {
		if (edid->header[i] == edid_header[i])
			score++;
	}

	return score;
}

int edid_block_compute_checksum(const void *_block)
{
	const uint8_t *block = _block;
	int i;
	uint8_t csum = 0, crc = 0;

	for (i = 0; i < EDID_LENGTH - 1; i++)
		csum = (uint8_t)(csum + block[i]);

	crc = (uint8_t)(0x100 - csum);

	return crc;
}

static int edid_block_get_checksum(const void *_block)
{
	const struct edid *block = _block;

	return block->checksum;
}

int edid_block_tag(const void *_block)
{
	const uint8_t *block = _block;

	return block[0];
}

static bool edid_block_is_zero(const void *edid)
{
	return edid_mem_is_zero(edid, EDID_LENGTH);
}

/* The checksum alone: the bytes of the block sum to zero. */
int drm_edid_block_valid(const uint8_t *block)
{
	return edid_block_compute_checksum(block) == edid_block_get_checksum(block);
}

enum edid_block_status edid_block_check(const void *_block, bool is_base_block)
{
	const struct edid *block = _block;

	if (!block)
		return EDID_BLOCK_NULL;

	if (is_base_block) {
		int score = drm_edid_header_is_valid(block);

		if (score < EDID_HEADER_FIXUP_MIN) {
			if (edid_block_is_zero(block))
				return EDID_BLOCK_ZERO;
			else
				return EDID_BLOCK_HEADER_CORRUPT;
		}

		if (score < 8)
			return EDID_BLOCK_HEADER_REPAIR;
	}

	if (edid_block_compute_checksum(block) != edid_block_get_checksum(block)) {
		if (edid_block_is_zero(block))
			return EDID_BLOCK_ZERO;
		else
			return EDID_BLOCK_CHECKSUM;
	}

	if (is_base_block) {
		if (block->version != 1)
			return EDID_BLOCK_VERSION;
	}

	return EDID_BLOCK_OK;
}

/*
 * A CTA extension with a wrong checksum is still used: televisions are
 * known to ship one, and the data blocks are bounds-checked anyway.
 */
bool edid_block_status_valid(enum edid_block_status status, int tag)
{
	return status == EDID_BLOCK_OK ||
	       status == EDID_BLOCK_HEADER_FIXED ||
	       (status == EDID_BLOCK_CHECKSUM && tag == CEA_EXT);
}

bool edid_block_valid(const void *block, bool base)
{
	return edid_block_status_valid(edid_block_check(block, base),
				       edid_block_tag(block));
}

/*
 * Validate a base or extension block, repairing a damaged base block
 * header in place.
 */
static bool edid_block_check_repair(void *_block, int block_num)
{
	struct edid *block = _block;
	enum edid_block_status status;
	bool is_base_block = block_num == 0;

	if (!block)
		return false;

	status = edid_block_check(block, is_base_block);
	if (status == EDID_BLOCK_HEADER_REPAIR) {
		edid_header_fix(block);

		/* Retry with fixed header, update status if that worked. */
		status = edid_block_check(block, is_base_block);
		if (status == EDID_BLOCK_OK)
			status = EDID_BLOCK_HEADER_FIXED;
	}

	/* Determine whether we can use this block with this status. */
	return edid_block_status_valid(status, edid_block_tag(block));
}

bool drm_edid_is_valid(struct edid *edid)
{
	int i;

	if (!edid)
		return false;

	for (i = 0; i < edid_block_count(edid); i++) {
		void *block = (void *)edid_block_data(edid, i);

		if (!edid_block_check_repair(block, i))
			return false;
	}

	return true;
}

bool drm_edid_valid(const struct drm_edid *drm_edid)
{
	int i;

	if (!drm_edid)
		return false;

	if ((size_t)edid_size_by_blocks(__drm_edid_block_count(drm_edid)) != drm_edid->size)
		return false;

	for (i = 0; i < drm_edid_block_count(drm_edid); i++) {
		const void *block = drm_edid_block_data(drm_edid, i);

		if (!edid_block_valid(block, i == 0))
			return false;
	}

	return true;
}

/*
 * Note: If the EDID uses HF-EEODB, but has invalid blocks, we'll revert
 * back to regular extension count here. We don't want to start
 * modifying the HF-EEODB extension too.
 */
int edid_filter_invalid_blocks(struct edid *edid, int num_blocks)
{
	int i, valid_blocks = 0;

	for (i = 0; i < num_blocks; i++) {
		const void *src_block = edid_block_data(edid, i);

		if (edid_block_valid(src_block, i == 0)) {
			void *dst_block = (void *)edid_block_data(edid, valid_blocks);

			edid_memmove(dst_block, src_block, EDID_LENGTH);
			valid_blocks++;
		} else if (i == 0) {
			/* Extensions without their base block are nothing. */
			return 0;
		}
	}

	edid->extensions = (uint8_t)(valid_blocks - 1);
	edid->checksum = (uint8_t)edid_block_compute_checksum(edid);

	return valid_blocks;
}

/*
 * Get the HF-EEODB override extension block count from EDID.
 *
 * The passed in EDID may be partially read, as long as it has at least two
 * blocks (base block and one extension block) if EDID extension count is > 0.
 *
 * Note that this is *not* how you should parse CTA Data Blocks in general; this
 * is only to handle partially read EDIDs. Normally, use the CTA Data Block
 * iterators instead.
 *
 * References:
 * - HDMI 2.1 section 10.3.6 HDMI Forum EDID Extension Override Data Block
 */
static int edid_hfeeodb_extension_block_count(const struct edid *edid)
{
	const uint8_t *cta;

	/* No extensions according to base block, no HF-EEODB. */
	if (!edid_extension_block_count(edid))
		return 0;

	/* HF-EEODB is always in the first EDID extension block only */
	cta = edid_extension_block_data(edid, 0);
	if (edid_block_tag(cta) != CEA_EXT || cea_revision(cta) < 3)
		return 0;

	/* Need to have the data block collection, and at least 3 bytes. */
	if (cta[2] < 4 || cta[2] > 127 || cta[2] - 4 < 3)
		return 0;

	/*
	 * Sinks that include the HF-EEODB in their E-EDID shall include one and
	 * only one instance of the HF-EEODB in the E-EDID, occupying bytes 4
	 * through 6 of Block 1 of the E-EDID.
	 */
	if (!cea_db_is_hdmi_forum_eeodb(&cta[4]))
		return 0;

	return cta[4 + 2];
}

int edid_hfeeodb_block_count(const struct edid *edid)
{
	int eeodb = edid_hfeeodb_extension_block_count(edid);

	return eeodb ? eeodb + 1 : 0;
}

/*** containers ***/

/* A container around a copy of the data. */
const struct drm_edid *drm_edid_alloc(const void *edid, size_t size)
{
	struct drm_edid *drm_edid;
	void *copy;

	if (!edid || !size || size < EDID_LENGTH)
		return NULL;

	copy = kalloc(size);
	if (!copy)
		return NULL;
	edid_memcpy(copy, edid, size);

	drm_edid = kalloc(sizeof(*drm_edid));
	if (!drm_edid) {
		kfree(copy);
		return NULL;
	}
	drm_edid->edid = copy;
	drm_edid->size = size;

	return drm_edid;
}

/*
 * The same filtering the DDC reader applies to what it reads: a damaged
 * header repaired, extension blocks that fail their check dropped (the
 * extension count and checksum of the copy adjusted), only whole blocks
 * kept.  NULL when the base block cannot be used.
 */
const struct drm_edid *drm_edid_alloc_checked(const void *edid, size_t size)
{
	const struct drm_edid *drm_edid;
	struct edid *raw;
	int stored, announced, num_blocks, i;
	bool all_valid = true;

	if (!edid || size < EDID_LENGTH)
		return NULL;

	size -= size % EDID_LENGTH;
	drm_edid = drm_edid_alloc(edid, size);
	if (!drm_edid)
		return NULL;
	raw = (struct edid *)drm_edid->edid;

	if (!edid_block_check_repair(raw, 0)) {
		drm_edid_free(drm_edid);
		return NULL;
	}

	stored = (int)(size / EDID_LENGTH);
	announced = __drm_edid_block_count(drm_edid);
	num_blocks = min(stored, announced);

	for (i = 1; i < num_blocks; i++) {
		if (!edid_block_valid(edid_block_data(raw, i), false))
			all_valid = false;
	}
	if (!all_valid) {
		num_blocks = edid_filter_invalid_blocks(raw, num_blocks);
		if (!num_blocks) {
			drm_edid_free(drm_edid);
			return NULL;
		}
	}
	/* Keep only the blocks parsed; the stored tail past the announced
	 * count is not part of the EDID. */
	((struct drm_edid *)drm_edid)->size = (size_t)edid_size_by_blocks(num_blocks);

	return drm_edid;
}

const struct drm_edid *drm_edid_dup(const struct drm_edid *drm_edid)
{
	if (!drm_edid)
		return NULL;

	return drm_edid_alloc(drm_edid->edid, drm_edid->size);
}

void drm_edid_free(const struct drm_edid *drm_edid)
{
	if (!drm_edid)
		return;

	kfree((void *)drm_edid->edid);
	kfree((void *)drm_edid);
}

const struct edid *drm_edid_raw(const struct drm_edid *drm_edid)
{
	if (!drm_edid || !drm_edid->size)
		return NULL;

	/*
	 * Do not return pointers where relying on EDID extension count would
	 * lead to buffer overflow.
	 */
	if ((size_t)edid_size(drm_edid->edid) > drm_edid->size)
		return NULL;

	return drm_edid->edid;
}

/*
 * Search EDID for an extension block with the given tag, from *ext_index
 * on; *ext_index is left past the block found.
 */
const uint8_t *drm_edid_find_extension(const struct drm_edid *drm_edid,
				       int ext_id, int *ext_index)
{
	const uint8_t *edid_ext = NULL;
	int i;

	/* No EDID or EDID extensions */
	if (!drm_edid || !drm_edid_extension_block_count(drm_edid))
		return NULL;

	/* Find the extension */
	for (i = *ext_index; i < drm_edid_extension_block_count(drm_edid); i++) {
		edid_ext = drm_edid_extension_block_data(drm_edid, i);
		if (edid_block_tag(edid_ext) == ext_id)
			break;
	}

	if (i >= drm_edid_extension_block_count(drm_edid))
		return NULL;

	*ext_index = i + 1;

	return edid_ext;
}

/*** identity ***/

void drm_edid_get_product_id(const struct drm_edid *drm_edid,
			     struct drm_edid_product_id *id)
{
	if (drm_edid && drm_edid->edid && drm_edid->size >= EDID_LENGTH)
		edid_memcpy(id, &drm_edid->edid->product_id, sizeof(*id));
	else
		edid_memset(id, 0, sizeof(*id));
}

/*
 * The panel id: 16 bits of manufacturer, 16 of product.  The product half
 * is read little endian as everyone does; the manufacturer half is kept
 * big endian, which makes drm_edid_encode_panel_id() easy to write.
 */
uint32_t drm_edid_get_panel_id(const struct drm_edid *drm_edid)
{
	const struct edid *edid = drm_edid->edid;

	if (drm_edid->size < EDID_LENGTH)
		return 0;

	return (uint32_t)edid->mfg_id[0] << 24 |
	       (uint32_t)edid->mfg_id[1] << 16 |
	       (uint32_t)EDID_PRODUCT_ID(edid);
}

bool drm_edid_is_digital(const struct drm_edid *drm_edid)
{
	return drm_edid && drm_edid->edid &&
	       (drm_edid->edid->input & DRM_EDID_INPUT_DIGITAL);
}

struct drm_edid_match_closure {
	const struct drm_edid_ident *ident;
	bool matched;
};

static bool edid_isspace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static void match_identity(const struct detailed_timing *timing, void *data)
{
	struct drm_edid_match_closure *closure = data;
	unsigned int i;
	const char *name = closure->ident->name;
	unsigned int name_len = 0;
	const char *desc = (const char *)timing->data.other_data.data.str.str;
	unsigned int desc_len = sizeof(timing->data.other_data.data.str.str);

	while (name[name_len])
		name_len++;

	if (name_len > desc_len ||
	    !(is_display_descriptor(timing, EDID_DETAIL_MONITOR_NAME) ||
	      is_display_descriptor(timing, EDID_DETAIL_MONITOR_STRING)))
		return;

	if (edid_memcmp(name, desc, name_len))
		return;

	for (i = name_len; i < desc_len; i++) {
		if (desc[i] == '\n')
			break;
		/* Allow white space before EDID string terminator. */
		if (!edid_isspace(desc[i]))
			return;
	}

	closure->matched = true;
}

bool drm_edid_match(const struct drm_edid *drm_edid,
		    const struct drm_edid_ident *ident)
{
	if (!drm_edid || drm_edid_get_panel_id(drm_edid) != ident->panel_id)
		return false;

	/* Match with name only if it's not NULL. */
	if (ident->name) {
		struct drm_edid_match_closure closure = {
			.ident = ident,
			.matched = false,
		};

		drm_for_each_detailed_block(drm_edid, match_identity, &closure);

		return closure.matched;
	}

	return true;
}

/*
 * The quirk bits of the sink: what subsequent routines need to fix.
 */
uint32_t edid_get_quirks(const struct drm_edid *drm_edid)
{
	const struct edid_quirk *quirk;
	int i;

	for (i = 0; i < edid_tbl_quirk_count; i++) {
		quirk = &edid_tbl_quirk_list[i];
		if (drm_edid_match(drm_edid, &quirk->ident))
			return quirk->quirks;
	}

	return 0;
}

/*** descriptors ***/

static void cea_for_each_detailed_block(const uint8_t *ext, detailed_cb *cb, void *closure)
{
	int i, n;
	uint8_t d = ext[0x02];
	const uint8_t *det_base = ext + d;

	if (d < 4 || d > 127)
		return;

	n = (127 - d) / 18;
	for (i = 0; i < n; i++)
		cb((const struct detailed_timing *)(det_base + 18 * i), closure);
}

static void vtb_for_each_detailed_block(const uint8_t *ext, detailed_cb *cb, void *closure)
{
	unsigned int i, n = (unsigned int)min((int)ext[0x02], 6);
	const uint8_t *det_base = ext + 5;

	if (ext[0x01] != 1)
		return; /* unknown version */

	for (i = 0; i < n; i++)
		cb((const struct detailed_timing *)(det_base + 18 * i), closure);
}

void drm_for_each_detailed_block(const struct drm_edid *drm_edid,
				 detailed_cb *cb, void *closure)
{
	struct drm_edid_iter edid_iter;
	const uint8_t *ext;
	int i;

	if (!drm_edid)
		return;

	for (i = 0; i < EDID_DETAILED_TIMINGS; i++)
		cb(&drm_edid->edid->detailed_timings[i], closure);

	drm_edid_iter_begin(drm_edid, &edid_iter);
	drm_edid_iter_for_each(ext, &edid_iter) {
		switch (*ext) {
		case CEA_EXT:
			cea_for_each_detailed_block(ext, cb, closure);
			break;
		case VTB_EXT:
			vtb_for_each_detailed_block(ext, cb, closure);
			break;
		default:
			break;
		}
	}
	drm_edid_iter_end(&edid_iter);
}

static void monitor_name(const struct detailed_timing *timing, void *data)
{
	const char **res = data;

	if (!is_display_descriptor(timing, EDID_DETAIL_MONITOR_NAME))
		return;

	*res = (const char *)timing->data.other_data.data.str.str;
}

int get_monitor_name(const struct drm_edid *drm_edid, char name[13])
{
	const char *edid_name = NULL;
	int mnl;

	if (!drm_edid || !name)
		return 0;

	drm_for_each_detailed_block(drm_edid, monitor_name, &edid_name);
	for (mnl = 0; edid_name && mnl < 13; mnl++) {
		if (edid_name[mnl] == 0x0a)
			break;

		name[mnl] = edid_name[mnl];
	}

	return mnl;
}

void drm_edid_get_monitor_name(const struct edid *edid, char *name, int bufsize)
{
	int name_length = 0;

	if (bufsize <= 0)
		return;

	if (edid) {
		char buf[13];
		struct drm_edid drm_edid = {
			.edid = edid,
			.size = (size_t)edid_size(edid),
		};

		name_length = min(get_monitor_name(&drm_edid, buf), bufsize - 1);
		edid_memcpy(name, buf, (size_t)name_length);
	}

	name[name_length] = '\0';
}

/*** CTA data blocks ***/

bool cea_db_is_extended_tag(const struct cea_db *db, int tag)
{
	return cea_db_tag(db) == CTA_DB_EXTENDED_TAG &&
	       cea_db_payload_len(db) >= 1 &&
	       db->data[0] == tag;
}

bool cea_db_is_vendor(const struct cea_db *db, int vendor_oui)
{
	const uint8_t *data = cea_db_data(db);

	return cea_db_tag(db) == CTA_DB_VENDOR &&
	       cea_db_payload_len(db) >= 3 &&
	       oui(data[2], data[1], data[0]) == vendor_oui;
}

void cea_db_iter_edid_begin(const struct drm_edid *drm_edid, struct cea_db_iter *iter)
{
	edid_memset(iter, 0, sizeof(*iter));

	drm_edid_iter_begin(drm_edid, &iter->edid_iter);
	displayid_iter_edid_begin(drm_edid, &iter->displayid_iter);
}

static const struct cea_db *__cea_db_iter_current_block(const struct cea_db_iter *iter)
{
	const struct cea_db *db;

	if (!iter->collection)
		return NULL;

	db = (const struct cea_db *)&iter->collection[iter->index];

	if (iter->index + (int)sizeof(*db) <= iter->end &&
	    iter->index + (int)sizeof(*db) + cea_db_payload_len(db) <= iter->end)
		return db;

	return NULL;
}

/*
 * References:
 * - CTA-861-H section 7.3.3 CTA Extension Version 3
 */
static int cea_db_collection_size(const uint8_t *cta)
{
	uint8_t d = cta[2];

	if (d < 4 || d > 127)
		return 0;

	return d - 4;
}

/*
 * References:
 * - VESA E-EDID v1.4
 * - CTA-861-H section 7.3.3 CTA Extension Version 3
 */
static const void *__cea_db_iter_edid_next(struct cea_db_iter *iter)
{
	const uint8_t *ext;

	drm_edid_iter_for_each(ext, &iter->edid_iter) {
		int size;

		/* Only support CTA Extension revision 3+ */
		if (ext[0] != CEA_EXT || cea_revision(ext) < 3)
			continue;

		size = cea_db_collection_size(ext);
		if (!size)
			continue;

		iter->index = 4;
		iter->end = iter->index + size;

		return ext;
	}

	return NULL;
}

/*
 * References:
 * - DisplayID v1.3 Appendix C: CEA Data Block within a DisplayID Data Block
 * - DisplayID v2.0 section 4.10 CTA DisplayID Data Block
 *
 * Note that the above do not specify any connection between DisplayID Data
 * Block revision and CTA Extension versions.
 */
static const void *__cea_db_iter_displayid_next(struct cea_db_iter *iter)
{
	const struct displayid_block *block;

	displayid_iter_for_each(block, &iter->displayid_iter) {
		if (block->tag != DATA_BLOCK_CTA)
			continue;

		/*
		 * The displayid iterator has already verified the block bounds
		 * in displayid_iter_block().
		 */
		iter->index = (int)sizeof(*block);
		iter->end = iter->index + block->num_bytes;

		return block;
	}

	return NULL;
}

const struct cea_db *__cea_db_iter_next(struct cea_db_iter *iter)
{
	const struct cea_db *db;

	if (iter->collection) {
		/* Current collection should always be valid. */
		db = __cea_db_iter_current_block(iter);
		if (!db) {
			iter->collection = NULL;
			return NULL;
		}

		/* Next block in CTA Data Block Collection */
		iter->index += (int)sizeof(*db) + cea_db_payload_len(db);

		db = __cea_db_iter_current_block(iter);
		if (db)
			return db;
	}

	for (;;) {
		/*
		 * Find the next CTA Data Block Collection. First iterate all
		 * the EDID CTA Extensions, then all the DisplayID CTA blocks.
		 *
		 * Per DisplayID v1.3 Appendix B: DisplayID as an EDID
		 * Extension, it's recommended that DisplayID extensions are
		 * exposed after all of the CTA Extensions.
		 */
		iter->collection = __cea_db_iter_edid_next(iter);
		if (!iter->collection)
			iter->collection = __cea_db_iter_displayid_next(iter);

		if (!iter->collection)
			return NULL;

		db = __cea_db_iter_current_block(iter);
		if (db)
			return db;
	}
}

void cea_db_iter_end(struct cea_db_iter *iter)
{
	displayid_iter_end(&iter->displayid_iter);
	drm_edid_iter_end(&iter->edid_iter);

	edid_memset(iter, 0, sizeof(*iter));
}

bool cea_db_is_hdmi_vsdb(const struct cea_db *db)
{
	return cea_db_is_vendor(db, HDMI_IEEE_OUI) &&
	       cea_db_payload_len(db) >= 5;
}

bool cea_db_is_hdmi_forum_vsdb(const struct cea_db *db)
{
	return cea_db_is_vendor(db, HDMI_FORUM_IEEE_OUI) &&
	       cea_db_payload_len(db) >= 7;
}

bool cea_db_is_hdmi_forum_eeodb(const void *db)
{
	return cea_db_is_extended_tag(db, CTA_EXT_DB_HF_EEODB) &&
	       cea_db_payload_len(db) >= 2;
}

bool cea_db_is_microsoft_vsdb(const struct cea_db *db)
{
	return cea_db_is_vendor(db, MICROSOFT_IEEE_OUI) &&
	       cea_db_payload_len(db) == 21;
}

bool cea_db_is_vcdb(const struct cea_db *db)
{
	return cea_db_is_extended_tag(db, CTA_EXT_DB_VIDEO_CAP) &&
	       cea_db_payload_len(db) == 2;
}

bool cea_db_is_hdmi_forum_scdb(const struct cea_db *db)
{
	return cea_db_is_extended_tag(db, CTA_EXT_DB_HF_SCDB) &&
	       cea_db_payload_len(db) >= 7;
}

bool cea_db_is_y420cmdb(const struct cea_db *db)
{
	return cea_db_is_extended_tag(db, CTA_EXT_DB_420_VIDEO_CAP_MAP);
}

bool cea_db_is_y420vdb(const struct cea_db *db)
{
	return cea_db_is_extended_tag(db, CTA_EXT_DB_420_VIDEO_DATA);
}

bool cea_db_is_hdmi_hdr_metadata_block(const struct cea_db *db)
{
	return cea_db_is_extended_tag(db, CTA_EXT_DB_HDR_STATIC_METADATA) &&
	       cea_db_payload_len(db) >= 3;
}

/*** the probed mode list ***/

bool drm_edid_probed_add(struct drm_edid_conn *conn, const struct drm_display_mode *mode)
{
	if (!conn->modes || conn->nmodes >= conn->max_modes) {
		conn->dropped++;
		return false;
	}
	conn->modes[conn->nmodes++] = *mode;
	return true;
}
