// LikeOS -- EDID: what a display says about itself.
//
// The 128-byte base block a monitor or panel answers with over DDC or AUX,
// plus optional extension blocks (CTA-861 for televisions and HDMI sinks,
// DisplayID for panels and tiled monitors), parsed into the modes it
// supports and the facts a driver needs: its preferred timing, its
// physical size, its name, whether it is an HDMI sink, what colour depths
// and formats and clocks it takes, its audio formats.
//
// Two interfaces:
//  - drm_edid_parse() fills the fixed-size struct drm_edid_info the
//    drivers have always used (modes as client modeinfo, preferred index);
//  - struct drm_edid + struct drm_edid_conn: the whole parse -- the sink's
//    struct drm_display_info, its ELD, latency and tile data, and the full
//    mode list as struct drm_display_mode -- for the connector code.
//
// The parser is pure (no kernel dependencies besides the allocator) so the
// host test harness can run it against recorded blocks.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2007-2008 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_EDID_H
#define KERNEL_DEV_GPU_DRM_EDID_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/drm/drm_mode.h>

#define DRM_EDID_BLOCK 128
#define DRM_EDID_MAX_BLOCKS 4 /* base + up to three extensions */
#define DRM_EDID_MAX_MODES 64

struct drm_edid_range {
	uint32_t min_vrefresh, max_vrefresh; /* Hz, 0 = unknown */
	uint32_t min_hfreq, max_hfreq; /* kHz */
	uint32_t max_clock_khz; /* 0 = unknown */
};

struct drm_edid_info {
	uint8_t version, revision;
	char vendor[4]; /* three letters */
	uint16_t product;
	uint32_t serial;
	char name[14]; /* the monitor name descriptor, if any */
	uint32_t mm_width, mm_height; /* physical size, 0 = unknown */
	int digital;
	int bpc; /* bits per colour (drm_display_info.bpc), 0 = unknown */
	int is_hdmi; /* CEA extension carries the HDMI vendor block */
	int cea_underscan;
	int has_audio;
	struct drm_edid_range range;
	int has_range;
	struct drm_mode_modeinfo modes[DRM_EDID_MAX_MODES];
	int nmodes;
	int preferred; /* index into modes, -1 = none */
};

/* Parse `len' bytes (a whole number of blocks).  Returns 0, -EINVAL when
 * the base block is not an EDID (header beyond repair, bad checksum, not
 * version 1), or -ENOMEM (the parse works in a kalloc'ed buffer).  Extension blocks that fail their check are dropped, not
 * fatal (a CTA block with only a bad checksum is still used).
 *
 * The mode list is the full parse (drm_edid_add_modes) reduced to what a
 * client without stereo or picture-aspect support, on a source without
 * YCbCr 4:2:0 output, can be offered: stereo modes, pixel-doubled modes
 * and 4:2:0-only modes are left out, picture-aspect flags are cleared,
 * duplicates are merged, and the list is cut at DRM_EDID_MAX_MODES in the
 * order the EDID ranks them (detailed timings first).  `preferred' is the
 * mode the EDID marks preferred; with none marked, the first detailed
 * timing of an EDID before 1.4, else the largest mode. */
int drm_edid_parse(const uint8_t *edid, unsigned len, struct drm_edid_info *out);

/* The number of extension blocks the base block announces. */
static inline unsigned drm_edid_extensions(const uint8_t *edid)
{
	return edid[126];
}

/* One block's checksum: the bytes must sum to zero. */
int drm_edid_block_valid(const uint8_t *block);

/* ---- standard timings (drm_modes.c) --------------------------------- */

/* Compute a mode from its geometry with the CVT formula (reduced blanking
 * when `rb'), or the GTF formula.  Fills every field including the name. */
void drm_mode_cvt(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh, int rb);
void drm_mode_gtf(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh);
/* The standard table (DMT and the common CEA timings): entry i, or -1
 * past the end. */
int drm_mode_table_get(int i, struct drm_mode_modeinfo *m);
int drm_mode_table_count(void);
/* A CEA-861 video identification code's mode, 0 when unknown. */
int drm_mode_cea_vic(uint8_t vic, struct drm_mode_modeinfo *m);
/* Same geometry and timing (name and type ignored). */
int drm_mode_equal(const struct drm_mode_modeinfo *a,
		   const struct drm_mode_modeinfo *b);
/* Fill in vrefresh and name from the timings. */
void drm_mode_finish(struct drm_mode_modeinfo *m);
/* Is the mode within the sink's range and the caller's clock limit? */
int drm_mode_in_range(const struct drm_mode_modeinfo *m,
		      const struct drm_edid_range *r, uint32_t max_clock_khz,
		      uint32_t max_w, uint32_t max_h);

/* ==== the EDID layout ===================================================
 *
 * Byte layouts of the base block and its descriptors (VESA E-EDID 1.4).
 * Multi-byte fields are little endian, as the host is. */

#define EDID_LENGTH 128
#define DDC_ADDR 0x50
#define DDC_ADDR2 0x52 /* E-DDC 1.2 - where DisplayID can hide */

/* Extension block tags. */
#define CEA_EXT 0x02
#define VTB_EXT 0x10
#define DI_EXT 0x40
#define LS_EXT 0x50
#define MI_EXT 0x60
#define DISPLAYID_EXT 0x70

struct est_timings {
	uint8_t t1;
	uint8_t t2;
	uint8_t mfg_rsvd;
} __attribute__((packed));

/* 00=16:10, 01=4:3, 10=5:4, 11=16:9 */
#define EDID_TIMING_ASPECT_SHIFT 6
#define EDID_TIMING_ASPECT_MASK (0x3 << EDID_TIMING_ASPECT_SHIFT)

/* need to add 60 */
#define EDID_TIMING_VFREQ_SHIFT 0
#define EDID_TIMING_VFREQ_MASK (0x3f << EDID_TIMING_VFREQ_SHIFT)

struct std_timing {
	uint8_t hsize; /* need to multiply by 8 then add 248 */
	uint8_t vfreq_aspect;
} __attribute__((packed));

#define DRM_EDID_PT_HSYNC_POSITIVE (1 << 1)
#define DRM_EDID_PT_VSYNC_POSITIVE (1 << 2)
#define DRM_EDID_PT_SEPARATE_SYNC (3 << 3)
#define DRM_EDID_PT_STEREO (1 << 5)
#define DRM_EDID_PT_INTERLACED (1 << 7)

/* If detailed data is pixel timing */
struct detailed_pixel_timing {
	uint8_t hactive_lo;
	uint8_t hblank_lo;
	uint8_t hactive_hblank_hi;
	uint8_t vactive_lo;
	uint8_t vblank_lo;
	uint8_t vactive_vblank_hi;
	uint8_t hsync_offset_lo;
	uint8_t hsync_pulse_width_lo;
	uint8_t vsync_offset_pulse_width_lo;
	uint8_t hsync_vsync_offset_pulse_width_hi;
	uint8_t width_mm_lo;
	uint8_t height_mm_lo;
	uint8_t width_height_mm_hi;
	uint8_t hborder;
	uint8_t vborder;
	uint8_t misc;
} __attribute__((packed));

/* If it's not pixel timing, it'll be one of the below */
struct detailed_data_string {
	uint8_t str[13];
} __attribute__((packed));

#define DRM_EDID_RANGE_OFFSET_MIN_VFREQ (1 << 0) /* 1.4 */
#define DRM_EDID_RANGE_OFFSET_MAX_VFREQ (1 << 1) /* 1.4 */
#define DRM_EDID_RANGE_OFFSET_MIN_HFREQ (1 << 2) /* 1.4 */
#define DRM_EDID_RANGE_OFFSET_MAX_HFREQ (1 << 3) /* 1.4 */

#define DRM_EDID_DEFAULT_GTF_SUPPORT_FLAG 0x00 /* 1.3 */
#define DRM_EDID_RANGE_LIMITS_ONLY_FLAG 0x01 /* 1.4 */
#define DRM_EDID_SECONDARY_GTF_SUPPORT_FLAG 0x02 /* 1.3 */
#define DRM_EDID_CVT_SUPPORT_FLAG 0x04 /* 1.4 */

#define DRM_EDID_CVT_FLAGS_STANDARD_BLANKING (1 << 3)
#define DRM_EDID_CVT_FLAGS_REDUCED_BLANKING (1 << 4)

/* Quirks a driver may ask about (drm_edid_has_quirk); the EDID code's own
 * quirks are numbered after these. */
enum drm_edid_quirk {
	/* Do a dummy read before DPCD accesses, to prevent corruption. */
	DRM_EDID_QUIRK_DP_DPCD_PROBE,

	DRM_EDID_QUIRK_NUM,
};

struct detailed_data_monitor_range {
	uint8_t min_vfreq;
	uint8_t max_vfreq;
	uint8_t min_hfreq_khz;
	uint8_t max_hfreq_khz;
	uint8_t pixel_clock_mhz; /* need to multiply by 10 */
	uint8_t flags;
	union {
		struct {
			uint8_t reserved;
			uint8_t hfreq_start_khz; /* need to multiply by 2 */
			uint8_t c; /* need to divide by 2 */
			uint16_t m;
			uint8_t k;
			uint8_t j; /* need to divide by 2 */
		} __attribute__((packed)) gtf2;
		struct {
			uint8_t version;
			uint8_t data1; /* high 6 bits: extra clock resolution */
			uint8_t data2; /* plus low 2 of above: max hactive */
			uint8_t supported_aspects;
			uint8_t flags; /* preferred aspect and blanking support */
			uint8_t supported_scalings;
			uint8_t preferred_refresh;
		} __attribute__((packed)) cvt;
	} __attribute__((packed)) formula;
} __attribute__((packed));

struct detailed_data_wpindex {
	uint8_t white_yx_lo; /* Lower 2 bits each */
	uint8_t white_x_hi;
	uint8_t white_y_hi;
	uint8_t gamma; /* need to divide by 100 then add 1 */
} __attribute__((packed));

struct detailed_data_color_point {
	uint8_t windex1;
	uint8_t wpindex1[3];
	uint8_t windex2;
	uint8_t wpindex2[3];
} __attribute__((packed));

struct cvt_timing {
	uint8_t code[3];
} __attribute__((packed));

struct detailed_non_pixel {
	uint8_t pad1;
	uint8_t type; /* ff=serial, fe=string, fd=monitor range, fc=monitor name
		       * fb=color point data, fa=standard timing data,
		       * f9=undefined, f8=mfg. reserved */
	uint8_t pad2;
	union {
		struct detailed_data_string str;
		struct detailed_data_monitor_range range;
		struct detailed_data_wpindex color;
		struct std_timing timings[6];
		struct cvt_timing cvt[4];
	} __attribute__((packed)) data;
} __attribute__((packed));

#define EDID_DETAIL_EST_TIMINGS 0xf7
#define EDID_DETAIL_CVT_3BYTE 0xf8
#define EDID_DETAIL_COLOR_MGMT_DATA 0xf9
#define EDID_DETAIL_STD_MODES 0xfa
#define EDID_DETAIL_MONITOR_CPDATA 0xfb
#define EDID_DETAIL_MONITOR_NAME 0xfc
#define EDID_DETAIL_MONITOR_RANGE 0xfd
#define EDID_DETAIL_MONITOR_STRING 0xfe
#define EDID_DETAIL_MONITOR_SERIAL 0xff

struct detailed_timing {
	uint16_t pixel_clock; /* need to multiply by 10 KHz */
	union {
		struct detailed_pixel_timing pixel_data;
		struct detailed_non_pixel other_data;
	} __attribute__((packed)) data;
} __attribute__((packed));

#define DRM_EDID_INPUT_SERRATION_VSYNC (1 << 0)
#define DRM_EDID_INPUT_SYNC_ON_GREEN (1 << 1)
#define DRM_EDID_INPUT_COMPOSITE_SYNC (1 << 2)
#define DRM_EDID_INPUT_SEPARATE_SYNCS (1 << 3)
#define DRM_EDID_INPUT_BLANK_TO_BLACK (1 << 4)
#define DRM_EDID_INPUT_VIDEO_LEVEL (3 << 5)
#define DRM_EDID_INPUT_DIGITAL (1 << 7)
#define DRM_EDID_DIGITAL_DEPTH_MASK (7 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_UNDEF (0 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_6 (1 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_8 (2 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_10 (3 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_12 (4 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_14 (5 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_16 (6 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_DEPTH_RSVD (7 << 4) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_MASK (7 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_UNDEF (0 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_DVI (1 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_HDMI_A (2 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_HDMI_B (3 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_MDDI (4 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_TYPE_DP (5 << 0) /* 1.4 */
#define DRM_EDID_DIGITAL_DFP_1_X (1 << 0) /* 1.3 */

#define DRM_EDID_FEATURE_DEFAULT_GTF (1 << 0) /* 1.2 */
#define DRM_EDID_FEATURE_CONTINUOUS_FREQ (1 << 0) /* 1.4 */
#define DRM_EDID_FEATURE_PREFERRED_TIMING (1 << 1)
#define DRM_EDID_FEATURE_STANDARD_COLOR (1 << 2)
/* If analog */
#define DRM_EDID_FEATURE_DISPLAY_TYPE (3 << 3) /* 00=mono, 01=rgb, 10=non-rgb, 11=unknown */
/* If digital */
#define DRM_EDID_FEATURE_COLOR_MASK (3 << 3)
#define DRM_EDID_FEATURE_RGB (0 << 3)
#define DRM_EDID_FEATURE_RGB_YCRCB444 (1 << 3)
#define DRM_EDID_FEATURE_RGB_YCRCB422 (2 << 3)
#define DRM_EDID_FEATURE_RGB_YCRCB (3 << 3) /* both 4:4:4 and 4:2:2 */

#define DRM_EDID_FEATURE_PM_ACTIVE_OFF (1 << 5)
#define DRM_EDID_FEATURE_PM_SUSPEND (1 << 6)
#define DRM_EDID_FEATURE_PM_STANDBY (1 << 7)

#define DRM_EDID_HDMI_DC_48 (1 << 6)
#define DRM_EDID_HDMI_DC_36 (1 << 5)
#define DRM_EDID_HDMI_DC_30 (1 << 4)
#define DRM_EDID_HDMI_DC_Y444 (1 << 3)

/* YCBCR 420 deep color modes */
#define DRM_EDID_YCBCR420_DC_48 (1 << 2)
#define DRM_EDID_YCBCR420_DC_36 (1 << 1)
#define DRM_EDID_YCBCR420_DC_30 (1 << 0)
#define DRM_EDID_YCBCR420_DC_MASK \
	(DRM_EDID_YCBCR420_DC_48 | DRM_EDID_YCBCR420_DC_36 | DRM_EDID_YCBCR420_DC_30)

/* HDMI 2.1 additional fields */
#define DRM_EDID_MAX_FRL_RATE_MASK 0xf0
#define DRM_EDID_FAPA_START_LOCATION (1 << 0)
#define DRM_EDID_ALLM (1 << 1)
#define DRM_EDID_FVA (1 << 2)

/* Deep Color specific */
#define DRM_EDID_DC_30BIT_420 (1 << 0)
#define DRM_EDID_DC_36BIT_420 (1 << 1)
#define DRM_EDID_DC_48BIT_420 (1 << 2)

/* VRR specific */
#define DRM_EDID_CNMVRR (1 << 3)
#define DRM_EDID_CINEMA_VRR (1 << 4)
#define DRM_EDID_MDELTA (1 << 5)
#define DRM_EDID_VRR_MAX_UPPER_MASK 0xc0
#define DRM_EDID_VRR_MAX_LOWER_MASK 0xff
#define DRM_EDID_VRR_MIN_MASK 0x3f

/* DSC specific */
#define DRM_EDID_DSC_10BPC (1 << 0)
#define DRM_EDID_DSC_12BPC (1 << 1)
#define DRM_EDID_DSC_16BPC (1 << 2)
#define DRM_EDID_DSC_ALL_BPP (1 << 3)
#define DRM_EDID_DSC_NATIVE_420 (1 << 6)
#define DRM_EDID_DSC_1P2 (1 << 7)
#define DRM_EDID_DSC_MAX_FRL_RATE_MASK 0xf0
#define DRM_EDID_DSC_MAX_SLICES 0xf
#define DRM_EDID_DSC_TOTAL_CHUNK_KBYTES 0x3f

struct drm_edid_product_id {
	uint16_t manufacturer_name; /* big endian */
	uint16_t product_code;
	uint32_t serial_number;
	uint8_t week_of_manufacture;
	uint8_t year_of_manufacture;
} __attribute__((packed));

struct edid {
	uint8_t header[8];
	/* Vendor & product info */
	union {
		struct drm_edid_product_id product_id;
		struct {
			uint8_t mfg_id[2];
			uint8_t prod_code[2];
			uint32_t serial;
			uint8_t mfg_week;
			uint8_t mfg_year;
		} __attribute__((packed));
	} __attribute__((packed));
	/* EDID version */
	uint8_t version;
	uint8_t revision;
	/* Display info: */
	uint8_t input;
	uint8_t width_cm;
	uint8_t height_cm;
	uint8_t gamma;
	uint8_t features;
	/* Color characteristics */
	uint8_t red_green_lo;
	uint8_t blue_white_lo;
	uint8_t red_x;
	uint8_t red_y;
	uint8_t green_x;
	uint8_t green_y;
	uint8_t blue_x;
	uint8_t blue_y;
	uint8_t white_x;
	uint8_t white_y;
	/* Est. timings and mfg rsvd timings*/
	struct est_timings established_timings;
	/* Standard timings 1-8*/
	struct std_timing standard_timings[8];
	/* Detailing timings 1-4 */
	struct detailed_timing detailed_timings[4];
	/* Number of 128 byte ext. blocks */
	uint8_t extensions;
	/* Checksum */
	uint8_t checksum;
} __attribute__((packed));

_Static_assert(sizeof(struct edid) == EDID_LENGTH, "EDID base block layout");

#define EDID_PRODUCT_ID(e) ((e)->prod_code[0] | ((e)->prod_code[1] << 8))

/* ==== EDID container ====================================================
 *
 * A whole EDID: the raw blocks and the size of the buffer holding them.
 * The block count the base block (or an HDMI forum EEODB) announces may
 * exceed what is stored; everything here reads only what is stored. */
struct drm_edid {
	size_t size; /* bytes of `edid', a multiple of EDID_LENGTH */
	const struct edid *edid;
};

/* EDID matching: a panel id (drm_edid_encode_panel_id) and optionally the
 * monitor name descriptor. */
struct drm_edid_ident {
	uint32_t panel_id;
	const char *name;
};

#define DRM_EDID_IDENT_INIT(_vend_chr_0, _vend_chr_1, _vend_chr_2, _product_id, _name) \
	{ \
		.panel_id = drm_edid_encode_panel_id(_vend_chr_0, _vend_chr_1, \
						     _vend_chr_2, _product_id), \
		.name = _name, \
	}

/* The three-letter manufacturer code: `vend' takes 4 bytes. */
static inline const char *drm_edid_decode_mfg_id(uint16_t mfg_id, char vend[4])
{
	vend[0] = (char)('@' + ((mfg_id >> 10) & 0x1f));
	vend[1] = (char)('@' + ((mfg_id >> 5) & 0x1f));
	vend[2] = (char)('@' + ((mfg_id >> 0) & 0x1f));
	vend[3] = '\0';

	return vend;
}

/* A 32-bit id per panel model: e.g. ('B', 'O', 'E', 0x2d08) => 0x09e52d08. */
#define drm_edid_encode_panel_id(vend_chr_0, vend_chr_1, vend_chr_2, product_id) \
	((((uint32_t)(vend_chr_0) - '@') & 0x1f) << 26 | \
	 (((uint32_t)(vend_chr_1) - '@') & 0x1f) << 21 | \
	 (((uint32_t)(vend_chr_2) - '@') & 0x1f) << 16 | \
	 ((product_id) & 0xffff))

static inline void drm_edid_decode_panel_id(uint32_t panel_id, char vend[4],
					    uint16_t *product_id)
{
	*product_id = (uint16_t)(panel_id & 0xffff);
	drm_edid_decode_mfg_id((uint16_t)(panel_id >> 16), vend);
}

/* Short Audio Descriptor */
struct cea_sad {
	uint8_t format;
	uint8_t channels; /* max number of channels - 1 */
	uint8_t freq;
	uint8_t byte2; /* meaning depends on format */
};

/* The RGB quantisation range of the AVI infoframe. */
enum hdmi_quantization_range {
	HDMI_QUANTIZATION_RANGE_DEFAULT,
	HDMI_QUANTIZATION_RANGE_LIMITED,
	HDMI_QUANTIZATION_RANGE_FULL,
	HDMI_QUANTIZATION_RANGE_RESERVED,
};

/* ==== the connector's view ==============================================
 *
 * What the EDID code reads and fills on behalf of a connector.  The caller
 * points `info', `eld' and `modes' at its own storage (the connector's
 * display_info and eld[], a mode array); the EDID code never keeps them.
 *
 *   drm_edid_update_display_info(conn, drm_edid);   info, eld, tile, latency
 *   n = drm_edid_add_modes(conn, drm_edid);          appends to modes[]
 *
 * add_modes reads info (quirks, video codes), so it comes second. */

#define DRM_ELD_BUF_SIZE 128

/* A tile of a monitor driven over several connectors (DisplayID). */
struct drm_edid_tile {
	bool has_tile;
	bool tile_is_single_monitor; /* the tiles are one physical monitor */
	uint8_t num_h_tile, num_v_tile; /* tiles in the group */
	uint8_t tile_h_loc, tile_v_loc; /* this tile's position */
	uint16_t tile_h_size, tile_v_size; /* this tile's size, pixels */
	uint8_t topology_id[8]; /* names the group */
};

struct drm_display_info;
struct drm_display_mode;

struct drm_edid_conn {
	/* DRM_MODE_CONNECTOR_* (the ELD says DisplayPort or HDMI). */
	uint32_t connector_type;
	/* Filled by drm_edid_update_display_info, which resets it first
	 * (freeing info->vics): it must start zeroed or be one this code
	 * filled before.  drm_edid_add_modes reads its quirks and vics. */
	struct drm_display_info *info;
	uint8_t *eld; /* DRM_ELD_BUF_SIZE bytes, or NULL for no ELD */
	bool latency_present[2]; /* progressive, interlaced */
	int video_latency[2];
	int audio_latency[2];
	struct drm_edid_tile tile;
	/* The probed modes: drm_edid_add_modes appends while nmodes <
	 * max_modes and counts what did not fit in `dropped'. */
	struct drm_display_mode *modes;
	int max_modes;
	int nmodes;
	int dropped;
};

/* ---- containers (drm_edid_core.c) --------------------------------------- */

/* The functions here that take a bare `struct edid *' trust its extension
 * count: the buffer must hold every block the base block announces.  For
 * a buffer that may hold fewer (DRM_EDID_MAX_BLOCKS of a longer EDID),
 * use a struct drm_edid. */

/* A copy of `size' bytes (at least one block) in a new container; NULL on
 * a short buffer or no memory.  Free with drm_edid_free(). */
const struct drm_edid *drm_edid_alloc(const void *edid, size_t size);
/* Like drm_edid_alloc, but only what passes the checks: a damaged header
 * is repaired, extension blocks that fail are dropped (the extension
 * count and checksum adjusted), and NULL when the base block is unusable.
 * The container to parse an EDID a driver read itself with. */
const struct drm_edid *drm_edid_alloc_checked(const void *edid, size_t size);
const struct drm_edid *drm_edid_dup(const struct drm_edid *drm_edid);
void drm_edid_free(const struct drm_edid *drm_edid);
/* Every stored block valid, and the size what the EDID announces. */
bool drm_edid_valid(const struct drm_edid *drm_edid);
/* The raw EDID, or NULL when its announced size exceeds the buffer. */
const struct edid *drm_edid_raw(const struct drm_edid *drm_edid);

/* 8 when the base block's header is perfect, down to 0. */
int drm_edid_header_is_valid(const void *edid);
/* Every block of a buffer sized by its own extension count is usable;
 * repairs a damaged header in place. */
bool drm_edid_is_valid(struct edid *edid);
bool drm_edid_is_digital(const struct drm_edid *drm_edid);
void drm_edid_get_product_id(const struct drm_edid *drm_edid,
			     struct drm_edid_product_id *id);
uint32_t drm_edid_get_panel_id(const struct drm_edid *drm_edid);
bool drm_edid_match(const struct drm_edid *drm_edid,
		    const struct drm_edid_ident *ident);
/* The monitor name descriptor, NUL-terminated in `name' (bufsize >= 14). */
void drm_edid_get_monitor_name(const struct edid *edid, char *name, int bufsize);

/* ---- sink information and modes (drm_edid_parse.c) ---------------------- */

/* Fill conn->info (reset first, freeing its video code list), the ELD,
 * the tile and latency data from the EDID; a NULL EDID just resets. */
void drm_edid_update_display_info(struct drm_edid_conn *conn,
				  const struct drm_edid *drm_edid);
/* Append the EDID's modes to conn->modes in the order the EDID ranks
 * them; returns the number found (including any that did not fit). */
int drm_edid_add_modes(struct drm_edid_conn *conn, const struct drm_edid *drm_edid);
/* The two above on a raw buffer (checked as drm_edid_alloc_checked):
 * the number of modes, or -EINVAL / -ENOMEM. */
int drm_edid_parse_full(const uint8_t *edid, unsigned len, struct drm_edid_conn *conn);
/* Clear a drm_display_info, freeing what the EDID code allocated in it. */
void drm_display_info_reset(struct drm_display_info *info);
/* Does the sink's EDID carry this driver-visible quirk? */
bool drm_edid_has_quirk(const struct drm_display_info *info, enum drm_edid_quirk quirk);

/* The DMT modes up to the given size (0 = any) at no more than 61 Hz, for
 * a connector without an EDID. */
int drm_add_modes_noedid(struct drm_edid_conn *conn, unsigned int hdisplay,
			 unsigned int vdisplay);
/* The CEA-861 mode of a video code; 0, or -EINVAL for an unknown code.
 * (Matching a mode to its code: drm_match_cea_mode() in drm_modes.h.) */
int drm_display_mode_from_cea_vic(uint8_t video_code, struct drm_display_mode *out);
/* The default RGB quantisation range for the mode (CEA-861). */
enum hdmi_quantization_range drm_default_rgb_quant_range(const struct drm_display_mode *mode);
/* Is the sink an HDMI sink / does it take audio (raw, sized EDID)? */
bool drm_detect_hdmi_monitor(const struct edid *edid);
bool drm_detect_monitor_audio(const struct edid *edid);

/* ---- audio (drm_eld.c) -------------------------------------------------- */

/* Build conn->eld from the EDID (conn->info->cea_rev must be filled). */
void drm_edid_to_eld(struct drm_edid_conn *conn, const struct drm_edid *drm_edid);
/* The short audio descriptors of the first audio block: the count, *sads
 * allocated (kfree), or a negative errno. */
int drm_edid_to_sad(const struct edid *edid, struct cea_sad **sads);
/* The speaker allocation block: its length, *sadb allocated (kfree). */
int drm_edid_to_speaker_allocation(const struct edid *edid, uint8_t **sadb);
/* Audio-to-video sync delay of the sink for the mode, milliseconds. */
int drm_av_sync_delay(const struct drm_edid_conn *conn,
		      const struct drm_display_mode *mode);
void drm_edid_cta_sad_get(const struct cea_sad *cta_sad, uint8_t *sad);
void drm_edid_cta_sad_set(struct cea_sad *cta_sad, const uint8_t *sad);

#endif
