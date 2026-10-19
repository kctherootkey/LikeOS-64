// LikeOS -- display-manager core: the DisplayID structure.
//
// DisplayID is the successor of the EDID descriptor format.  Inside an
// EDID it rides in extension blocks tagged DISPLAYID_EXT: a section header
// (structure version, size, product type or primary use) followed by data
// blocks -- detailed and formula timings, tiled display topology, display
// parameters, vendor blocks, embedded CTA data blocks.  The layouts and
// the iterator over the data blocks of every DisplayID section of an EDID
// (drm_displayid.c) are here.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Red Hat's and Intel's code: MIT
// Portions Copyright (C) 2014 Red Hat Inc.
// Portions Copyright (C) 2021 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_DISPLAYID_H
#define KERNEL_DEV_GPU_DRM_DISPLAYID_H

#include <kernel/uapi/types.h>

struct drm_edid;

#define VESA_IEEE_OUI 0x3a0292

/* DisplayID Structure versions */
#define DISPLAY_ID_STRUCTURE_VER_20 0x20

/* DisplayID Structure v1r2 Data Blocks */
#define DATA_BLOCK_PRODUCT_ID 0x00
#define DATA_BLOCK_DISPLAY_PARAMETERS 0x01
#define DATA_BLOCK_COLOR_CHARACTERISTICS 0x02
#define DATA_BLOCK_TYPE_1_DETAILED_TIMING 0x03
#define DATA_BLOCK_TYPE_2_DETAILED_TIMING 0x04
#define DATA_BLOCK_TYPE_3_SHORT_TIMING 0x05
#define DATA_BLOCK_TYPE_4_DMT_TIMING 0x06
#define DATA_BLOCK_VESA_TIMING 0x07
#define DATA_BLOCK_CEA_TIMING 0x08
#define DATA_BLOCK_VIDEO_TIMING_RANGE 0x09
#define DATA_BLOCK_PRODUCT_SERIAL_NUMBER 0x0a
#define DATA_BLOCK_GP_ASCII_STRING 0x0b
#define DATA_BLOCK_DISPLAY_DEVICE_DATA 0x0c
#define DATA_BLOCK_INTERFACE_POWER_SEQUENCING 0x0d
#define DATA_BLOCK_TRANSFER_CHARACTERISTICS 0x0e
#define DATA_BLOCK_DISPLAY_INTERFACE 0x0f
#define DATA_BLOCK_STEREO_DISPLAY_INTERFACE 0x10
#define DATA_BLOCK_TILED_DISPLAY 0x12
#define DATA_BLOCK_VENDOR_SPECIFIC 0x7f
#define DATA_BLOCK_CTA 0x81

/* DisplayID Structure v2r0 Data Blocks */
#define DATA_BLOCK_2_PRODUCT_ID 0x20
#define DATA_BLOCK_2_DISPLAY_PARAMETERS 0x21
#define DATA_BLOCK_2_TYPE_7_DETAILED_TIMING 0x22
#define DATA_BLOCK_2_TYPE_8_ENUMERATED_TIMING 0x23
#define DATA_BLOCK_2_TYPE_9_FORMULA_TIMING 0x24
#define DATA_BLOCK_2_DYNAMIC_VIDEO_TIMING 0x25
#define DATA_BLOCK_2_DISPLAY_INTERFACE_FEATURES 0x26
#define DATA_BLOCK_2_STEREO_DISPLAY_INTERFACE 0x27
#define DATA_BLOCK_2_TILED_DISPLAY_TOPOLOGY 0x28
#define DATA_BLOCK_2_CONTAINER_ID 0x29
#define DATA_BLOCK_2_TYPE_10_FORMULA_TIMING 0x2a
#define DATA_BLOCK_2_VENDOR_SPECIFIC 0x7e
#define DATA_BLOCK_2_CTA_DISPLAY_ID 0x81

/* DisplayID Structure v1r2 Product Type */
#define PRODUCT_TYPE_EXTENSION 0
#define PRODUCT_TYPE_TEST 1
#define PRODUCT_TYPE_PANEL 2
#define PRODUCT_TYPE_MONITOR 3
#define PRODUCT_TYPE_TV 4
#define PRODUCT_TYPE_REPEATER 5
#define PRODUCT_TYPE_DIRECT_DRIVE 6

/* DisplayID Structure v2r0 Display Product Primary Use Case (~Product Type) */
#define PRIMARY_USE_EXTENSION 0
#define PRIMARY_USE_TEST 1
#define PRIMARY_USE_GENERIC 2
#define PRIMARY_USE_TV 3
#define PRIMARY_USE_DESKTOP_PRODUCTIVITY 4
#define PRIMARY_USE_DESKTOP_GAMING 5
#define PRIMARY_USE_PRESENTATION 6
#define PRIMARY_USE_HEAD_MOUNTED_VR 7
#define PRIMARY_USE_HEAD_MOUNTED_AR 8

struct displayid_header {
	uint8_t rev;
	uint8_t bytes;
	uint8_t prod_id;
	uint8_t ext_count;
} __attribute__((packed));

struct displayid_block {
	uint8_t tag;
	uint8_t rev;
	uint8_t num_bytes;
} __attribute__((packed));

struct displayid_tiled_block {
	struct displayid_block base;
	uint8_t tile_cap;
	uint8_t topo[3];
	uint8_t tile_size[4];
	uint8_t tile_pixel_bezel[5];
	uint8_t topology_id[9];
} __attribute__((packed));

/* Multi-byte fields little endian, as the host is. */
struct displayid_detailed_timings_1 {
	uint8_t pixel_clock[3];
	uint8_t flags;
	uint16_t hactive;
	uint16_t hblank;
	uint16_t hsync;
	uint16_t hsw;
	uint16_t vactive;
	uint16_t vblank;
	uint16_t vsync;
	uint16_t vsw;
} __attribute__((packed));

struct displayid_detailed_timing_block {
	struct displayid_block base;
	struct displayid_detailed_timings_1 timings[];
} __attribute__((packed));

struct displayid_formula_timings_9 {
	uint8_t flags;
	uint16_t hactive;
	uint16_t vactive;
	uint8_t vrefresh;
} __attribute__((packed));

struct displayid_formula_timing_block {
	struct displayid_block base;
	struct displayid_formula_timings_9 timings[];
} __attribute__((packed));

#define DISPLAYID_DEVICE_TECH_UNSPECIFIED 0
#define DISPLAYID_DEVICE_TECH_LCD 1
#define DISPLAYID_DEVICE_TECH_OLED 2

#define DISPLAYID_DISPLAY_PARAMS_DEVICE_TECH (0x7 << 4) /* bits 6:4 */

struct displayid_display_params_block {
	struct displayid_block base;
	uint16_t horiz_image_size;
	uint16_t vert_image_size;
	uint16_t horiz_pixel_count;
	uint16_t vert_pixel_count;
	uint8_t features;
	uint8_t primary_color1[3];
	uint8_t primary_color2[3];
	uint8_t primary_color3[3];
	uint8_t white_point[3];
	uint16_t max_luminance_full;
	uint16_t max_luminance_10;
	uint16_t min_luminance;
	uint8_t color_depth_and_tech; /* [2:0] depth, [6:4] device tech, [7] theme */
	uint8_t gamma_eotf;
} __attribute__((packed));

#define DISPLAYID_VESA_MSO_OVERLAP 0x0f /* bits 3:0 */
#define DISPLAYID_VESA_MSO_MODE (0x3 << 5) /* bits 6:5 */

struct displayid_vesa_vendor_specific_block {
	struct displayid_block base;
	uint8_t oui[3];
	uint8_t data_structure_type;
	uint8_t mso;
} __attribute__((packed));

/*
 * DisplayID iteration: every data block of every DisplayID section of an
 * EDID, bounds-checked.
 *
 *	struct displayid_iter iter;
 *	const struct displayid_block *block;
 *
 *	displayid_iter_edid_begin(drm_edid, &iter);
 *	displayid_iter_for_each(block, &iter) {
 *		...
 *	}
 *	displayid_iter_end(&iter);
 *
 * The fields are the iterator's own.
 */
struct displayid_iter {
	const struct drm_edid *drm_edid;

	const uint8_t *section;
	int length;
	int idx;
	int ext_index;

	uint8_t version;
	uint8_t primary_use;

	uint8_t quirks;
};

void displayid_iter_edid_begin(const struct drm_edid *drm_edid,
			       struct displayid_iter *iter);
const struct displayid_block *__displayid_iter_next(struct displayid_iter *iter);
#define displayid_iter_for_each(__block, __iter) \
	while (((__block) = __displayid_iter_next(__iter)))
void displayid_iter_end(struct displayid_iter *iter);

/* DisplayID Structure Version/Revision from the Base Section. */
uint8_t displayid_version(const struct displayid_iter *iter);
/* DisplayID Primary Use Case (2.0+) or Product Type Identifier (1.0-1.3)
 * from the Base Section. */
uint8_t displayid_primary_use(const struct displayid_iter *iter);

#endif
