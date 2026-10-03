// LikeOS -- display-manager core: what the EDID files share.
//
// The EDID code is split over several files -- block handling and
// iteration (drm_edid_core.c), modes and sink information
// (drm_edid_parse.c), DisplayID (drm_displayid.c), audio (drm_eld.c), the
// constant tables (drm_edid_tables.c) and the DDC reader (drm_edid.c).
// This header is their common vocabulary: the block and data-block
// iterators, the quirk bits, the CTA-861 tag codes, the tables, and the
// mode list a connector's modes are collected in.  Nothing outside those
// files includes it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Luc Verhaegen's, Intel's, Red Hat's and Dennis Munsie's code: MIT
// Portions Copyright (C) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2010 Red Hat, Inc.
// Portions Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>

#ifndef KERNEL_DEV_GPU_DRM_DRM_EDID_INTERNAL_H
#define KERNEL_DEV_GPU_DRM_DRM_EDID_INTERNAL_H

#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/drm_displayid.h>
#include <kernel/dev/gpu/drm_eld.h>
#include <kernel/dev/gpu/gpu_util.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

/* The kernel allocator (kernel/mm/memory.h), declared here so the pure
 * files need no other kernel header; the host tests supply them. */
void *kalloc(size_t size);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);

/* ---- small helpers ------------------------------------------------------ */

static inline void edid_memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;

	while (n--)
		*d++ = (uint8_t)c;
}

static inline void edid_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n--)
		*d++ = *s++;
}

/* Copies front to back or back to front, so the ranges may overlap. */
static inline void edid_memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if (d == s || !n)
		return;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
}

static inline int edid_memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x < *y ? -1 : 1;
	return 0;
}

static inline bool edid_mem_is_zero(const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n--)
		if (*b++)
			return false;
	return true;
}

static inline int edid_abs(int v)
{
	return v < 0 ? -v : v;
}

static inline int oui(uint8_t first, uint8_t second, uint8_t third)
{
	return (first << 16) | (second << 8) | third;
}

/* Picoseconds per pixel at a kHz clock: two clocks this close are the same
 * clock as far as a CEA mode is concerned. */
#define KHZ2PICOS(a) (1000000000UL / (a))

#define EDID_EST_TIMINGS 16
#define EDID_STD_TIMINGS 8
#define EDID_DETAILED_TIMINGS 4

/* ---- quirks --------------------------------------------------------------
 *
 * EDID blocks out in the wild have a variety of bugs; the quirk list
 * (drm_edid_tables.c) names the sinks and what to do about them.  Bits
 * below DRM_EDID_QUIRK_NUM are the ones drivers may ask about. */
enum drm_edid_internal_quirk {
	/* First detailed mode wrong, use largest 60Hz mode */
	EDID_QUIRK_PREFER_LARGE_60 = DRM_EDID_QUIRK_NUM,
	/* Reported 135MHz pixel clock is too high, needs adjustment */
	EDID_QUIRK_135_CLOCK_TOO_HIGH,
	/* Prefer the largest mode at 75 Hz */
	EDID_QUIRK_PREFER_LARGE_75,
	/* Detail timing is in cm not mm */
	EDID_QUIRK_DETAILED_IN_CM,
	/* Detailed timing descriptors have bogus size values, so just take the
	 * maximum size and use that. */
	EDID_QUIRK_DETAILED_USE_MAXIMUM_SIZE,
	/* use +hsync +vsync for detailed mode */
	EDID_QUIRK_DETAILED_SYNC_PP,
	/* Force reduced-blanking timings for detailed modes */
	EDID_QUIRK_FORCE_REDUCED_BLANKING,
	/* Force 8bpc */
	EDID_QUIRK_FORCE_8BPC,
	/* Force 12bpc */
	EDID_QUIRK_FORCE_12BPC,
	/* Force 6bpc */
	EDID_QUIRK_FORCE_6BPC,
	/* Force 10bpc */
	EDID_QUIRK_FORCE_10BPC,
	/* Non desktop display (i.e. HMD) */
	EDID_QUIRK_NON_DESKTOP,
	/* Cap the DSC target bitrate to 15bpp */
	EDID_QUIRK_CAP_DSC_15BPP,
};

struct edid_quirk {
	struct drm_edid_ident ident;
	uint32_t quirks;
};

/* The quirk bits of the sink (0 for an unknown one). */
uint32_t edid_get_quirks(const struct drm_edid *drm_edid);

static inline bool drm_edid_has_internal_quirk(const struct drm_edid_conn *conn,
					       enum drm_edid_internal_quirk quirk)
{
	return (conn->info->quirks & BIT(quirk)) != 0;
}

/* ---- vendor and CTA constants ----------------------------------------- */

#define HDMI_IEEE_OUI 0x000c03
#define HDMI_FORUM_IEEE_OUI 0xc45dd8
#define MICROSOFT_IEEE_OUI 0xca125c

/* The CEC physical address a sink without one reports. */
#define CEC_PHYS_ADDR_INVALID 0xffff

/* CTA-861-G electro-optical transfer functions and metadata types. */
#define HDMI_EOTF_TRADITIONAL_GAMMA_SDR 0
#define HDMI_EOTF_TRADITIONAL_GAMMA_HDR 1
#define HDMI_EOTF_SMPTE_ST2084 2
#define HDMI_EOTF_BT_2100_HLG 3
#define HDMI_STATIC_METADATA_TYPE1 0

/* CTA-861-H Table 60 - CTA Tag Codes */
#define CTA_DB_AUDIO 1
#define CTA_DB_VIDEO 2
#define CTA_DB_VENDOR 3
#define CTA_DB_SPEAKER 4
#define CTA_DB_EXTENDED_TAG 7

/* CTA-861-H Table 62 - CTA Extended Tag Codes */
#define CTA_EXT_DB_VIDEO_CAP 0
#define CTA_EXT_DB_VENDOR 1
#define CTA_EXT_DB_HDR_STATIC_METADATA 6
#define CTA_EXT_DB_420_VIDEO_DATA 14
#define CTA_EXT_DB_420_VIDEO_CAP_MAP 15
#define CTA_EXT_DB_HF_EEODB 0x78
#define CTA_EXT_DB_HF_SCDB 0x79

#define EDID_BASIC_AUDIO (1 << 6)
#define EDID_CEA_YCRCB444 (1 << 5)
#define EDID_CEA_YCRCB422 (1 << 4)
#define EDID_CEA_VCDB_QS (1 << 6)

/* ---- tables (drm_edid_tables.c) ---------------------------------------- */

struct minimode {
	short w;
	short h;
	short r;
	short rb;
};

extern const struct edid_quirk edid_tbl_quirk_list[];
extern const int edid_tbl_quirk_count;
/* the established timings bits I and II, in bit order */
extern const struct drm_display_mode edid_tbl_est_modes[];
extern const int edid_tbl_est_count;
/* the established timings III descriptor bits */
extern const struct minimode edid_tbl_est3_modes[];
extern const int edid_tbl_est3_count;
/* geometries tried against a range descriptor's formula */
extern const struct minimode edid_tbl_extra_modes[];
extern const int edid_tbl_extra_count;

/* The VESA DMT, CEA-861 and HDMI 1.4 tables and matching are drm_modes.c's
 * (drm_modes.h). */

/* ---- blocks (drm_edid_core.c) ------------------------------------------ */

enum edid_block_status {
	EDID_BLOCK_OK = 0,
	EDID_BLOCK_READ_FAIL,
	EDID_BLOCK_NULL,
	EDID_BLOCK_ZERO,
	EDID_BLOCK_HEADER_CORRUPT,
	EDID_BLOCK_HEADER_REPAIR,
	EDID_BLOCK_HEADER_FIXED,
	EDID_BLOCK_CHECKSUM,
	EDID_BLOCK_VERSION,
};

/* Minimum number of matching header bytes for a base block to be repaired
 * rather than refused (0-8). */
#define EDID_HEADER_FIXUP_MIN 6

void edid_header_fix(void *edid);
int edid_block_compute_checksum(const void *block);
int edid_block_tag(const void *block);
enum edid_block_status edid_block_check(const void *block, bool is_base_block);
bool edid_block_status_valid(enum edid_block_status status, int tag);
bool edid_block_valid(const void *block, bool base);
int edid_extension_block_count(const struct edid *edid);
int edid_block_count(const struct edid *edid);
int edid_size_by_blocks(int num_blocks);
int edid_size(const struct edid *edid);
const void *edid_block_data(const struct edid *edid, int index);
/* The HF-EEODB override of the block count (0 = none); needs the first
 * extension block present when the base block announces any. */
int edid_hfeeodb_block_count(const struct edid *edid);
/* Drop every block that fails its check, fix the extension count and the
 * checksum; returns the new block count (0: the base block failed). */
int edid_filter_invalid_blocks(struct edid *edid, int num_blocks);

/* Block count the EDID announces (may exceed what is stored), and limited
 * by what is stored. */
int __drm_edid_block_count(const struct drm_edid *drm_edid);
int drm_edid_block_count(const struct drm_edid *drm_edid);
int drm_edid_extension_block_count(const struct drm_edid *drm_edid);
const void *drm_edid_block_data(const struct drm_edid *drm_edid, int index);
const void *drm_edid_extension_block_data(const struct drm_edid *drm_edid, int index);
/* The next extension block tagged ext_id from *ext_index on (updated). */
const uint8_t *drm_edid_find_extension(const struct drm_edid *drm_edid,
				       int ext_id, int *ext_index);
/* A container around a buffer sized by its own extension count. */
const struct drm_edid *drm_edid_legacy_init(struct drm_edid *drm_edid,
					    const struct edid *edid);

/* The base and extension blocks of an EDID, in order. */
struct drm_edid_iter {
	const struct drm_edid *drm_edid;
	/* Current block index. */
	int index;
};

void drm_edid_iter_begin(const struct drm_edid *drm_edid, struct drm_edid_iter *iter);
const void *__drm_edid_iter_next(struct drm_edid_iter *iter);
#define drm_edid_iter_for_each(__block, __iter) \
	while (((__block) = __drm_edid_iter_next(__iter)))
void drm_edid_iter_end(struct drm_edid_iter *iter);

/* Every 18-byte descriptor: the base block's four, then those of the CTA
 * and VTB extensions. */
typedef void detailed_cb(const struct detailed_timing *timing, void *closure);
void drm_for_each_detailed_block(const struct drm_edid *drm_edid,
				 detailed_cb *cb, void *closure);

static inline bool is_display_descriptor(const struct detailed_timing *descriptor,
					 uint8_t type)
{
	return descriptor->pixel_clock == 0 &&
	       descriptor->data.other_data.pad1 == 0 &&
	       descriptor->data.other_data.type == type;
}

static inline bool is_detailed_timing_descriptor(const struct detailed_timing *descriptor)
{
	return descriptor->pixel_clock != 0;
}

/* The monitor name descriptor's characters up to the newline; the count. */
int get_monitor_name(const struct drm_edid *drm_edid, char name[13]);

/* ---- CTA data blocks ----------------------------------------------------
 *
 * Every CTA data block of the EDID's CTA extensions (revision 3+), then of
 * the CTA blocks embedded in DisplayID, bounds-checked.
 *
 *	struct cea_db_iter iter;
 *	const struct cea_db *db;
 *
 *	cea_db_iter_edid_begin(drm_edid, &iter);
 *	cea_db_iter_for_each(db, &iter) {
 *		...
 *	}
 *	cea_db_iter_end(&iter);
 */
struct cea_db_iter {
	struct drm_edid_iter edid_iter;
	struct displayid_iter displayid_iter;

	/* Current Data Block Collection. */
	const uint8_t *collection;
	/* Current Data Block index in current collection. */
	int index;
	/* End index in current collection. */
	int end;
};

/* CTA-861-H section 7.4 CTA Data BLock Collection */
struct cea_db {
	uint8_t tag_length;
	uint8_t data[];
} __attribute__((packed));

static inline int cea_db_tag(const struct cea_db *db)
{
	return db->tag_length >> 5;
}

static inline int cea_db_payload_len(const void *_db)
{
	const struct cea_db *db = _db;

	return db->tag_length & 0x1f;
}

static inline const void *cea_db_data(const struct cea_db *db)
{
	return db->data;
}

static inline int cea_revision(const uint8_t *cea)
{
	/* The revision of the CTA extension, or of the DisplayID CTA data
	 * block -- DisplayID does not say which it means. */
	return cea[1];
}

void cea_db_iter_edid_begin(const struct drm_edid *drm_edid, struct cea_db_iter *iter);
const struct cea_db *__cea_db_iter_next(struct cea_db_iter *iter);
#define cea_db_iter_for_each(__db, __iter) \
	while (((__db) = __cea_db_iter_next(__iter)))
void cea_db_iter_end(struct cea_db_iter *iter);

bool cea_db_is_extended_tag(const struct cea_db *db, int tag);
bool cea_db_is_vendor(const struct cea_db *db, int vendor_oui);
bool cea_db_is_hdmi_vsdb(const struct cea_db *db);
bool cea_db_is_hdmi_forum_vsdb(const struct cea_db *db);
bool cea_db_is_hdmi_forum_eeodb(const void *db);
bool cea_db_is_microsoft_vsdb(const struct cea_db *db);
bool cea_db_is_vcdb(const struct cea_db *db);
bool cea_db_is_hdmi_forum_scdb(const struct cea_db *db);
bool cea_db_is_y420cmdb(const struct cea_db *db);
bool cea_db_is_y420vdb(const struct cea_db *db);
bool cea_db_is_hdmi_hdr_metadata_block(const struct cea_db *db);

/* HDMI vendor block latency fields. */
static inline bool hdmi_vsdb_latency_present(const uint8_t *db)
{
	return (db[8] & BIT(7)) != 0;
}

static inline bool hdmi_vsdb_i_latency_present(const uint8_t *db)
{
	return hdmi_vsdb_latency_present(db) && (db[8] & BIT(6));
}

/* ---- the probed mode list --------------------------------------------- */

/* Append a copy of `mode' to conn->modes; false (and counted in
 * conn->dropped) when the list is full. */
bool drm_edid_probed_add(struct drm_edid_conn *conn, const struct drm_display_mode *mode);

/* HDMI vendor block audio fields into the ELD and latency (drm_eld.c). */
void drm_parse_hdmi_vsdb_audio(struct drm_edid_conn *conn, const uint8_t *db);
/* Clear the ELD and the latency data. */
void clear_eld(struct drm_edid_conn *conn);

#endif
