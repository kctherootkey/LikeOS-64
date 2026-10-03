// LikeOS -- display-manager core: what a connected sink is capable of.
//
// Filled in from the sink's EDID when one is installed on the connector
// (drm_connector_set_edid) and cleared when it goes: physical size, colour
// depth and formats, whether it is an HDMI sink and what of HDMI it does
// (SCDC, scrambling, deep colour, YCbCr 4:2:0 modes, FRL, DSC), its refresh
// and luminance ranges, HDR static metadata, and the CEA video codes it
// lists.  Drivers read it to choose link and pixel settings; the mode
// validation reads it for 4:2:0 and clock limits.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (C) 2016 Intel Corporation
// SPDX-License-Identifier for the portions derived from Avionic Design's code: MIT
// Portions Copyright (C) 2012 Avionic Design GmbH

#ifndef KERNEL_DEV_GPU_DRM_DISPLAY_INFO_H
#define KERNEL_DEV_GPU_DRM_DISPLAY_INFO_H

#include <kernel/uapi/types.h>

/* The subpixel layout of the panel. */
enum subpixel_order {
	SubPixelUnknown = 0,
	SubPixelHorizontalRGB,
	SubPixelHorizontalBGR,
	SubPixelVerticalRGB,
	SubPixelVerticalBGR,
	SubPixelNone,
};

/* drm_display_info.color_formats */
#define DRM_COLOR_FORMAT_RGB444 (1 << 0)
#define DRM_COLOR_FORMAT_YCBCR444 (1 << 1)
#define DRM_COLOR_FORMAT_YCBCR422 (1 << 2)
#define DRM_COLOR_FORMAT_YCBCR420 (1 << 3)

/* HDMI 2.0 scrambling capability. */
struct drm_scrambling {
	bool supported; /* the sink can scramble */
	bool low_rates; /* ... also below 340 MHz */
};

/* The sink's Status and Control Data Channel. */
struct drm_scdc {
	bool supported;
	bool read_request; /* the sink can initiate a read request */
	struct drm_scrambling scrambling;
};

/* HDMI 2.1 DSC capabilities of the sink. */
struct drm_hdmi_dsc_cap {
	bool v_1p2; /* DSC 1.2 */
	bool native_420; /* compressed native 4:2:0 */
	bool all_bpp; /* every bpp between min and max */
	uint8_t bpc_supported; /* compressed bpc: 8, 10, 12 */
	uint8_t max_slices;
	int clk_per_slice; /* max pixel clock per slice, MHz */
	uint8_t max_lanes; /* FRL lanes for DSC */
	uint8_t max_frl_rate_per_lane; /* Gbps per lane for DSC */
	uint8_t total_chunk_kbytes; /* DSC chunk size, KB */
};

/* What the HDMI forum vendor block and the 4:2:0 blocks say.  The
 * y420 bitmaps are indexed by CEA VIC (256 bits each, in longs). */
struct drm_hdmi_info {
	struct drm_scdc scdc;
	/* VICs the sink takes ONLY as 4:2:0 (Y420VDB) */
	unsigned long y420_vdb_modes[256 / (8 * sizeof(unsigned long))];
	/* VICs the sink takes as 4:2:0 too (Y420CMDB) */
	unsigned long y420_cmdb_modes[256 / (8 * sizeof(unsigned long))];
	/* deep colour depths in 4:2:0 (DRM_EDID_YCBCR420_DC_*) */
	uint8_t y420_dc_modes;
	uint8_t max_frl_rate_per_lane; /* Gbps per lane, FRL */
	uint8_t max_lanes; /* FRL lanes */
	struct drm_hdmi_dsc_cap dsc_cap;
};

/* The vertical refresh range the sink accepts (Hz). */
struct drm_monitor_range_info {
	uint16_t min_vfreq;
	uint16_t max_vfreq;
};

/* The panel's luminance range (cd/m^2, from the DisplayID / HDR blocks). */
struct drm_luminance_range_info {
	uint32_t min_luminance;
	uint32_t max_luminance;
};

/* The HDR static metadata block of the sink (CTA-861-G). */
struct hdr_static_metadata {
	uint8_t eotf;
	uint8_t metadata_type;
	uint16_t max_cll;
	uint16_t max_fall;
	uint16_t min_cll;
};

struct hdr_sink_metadata {
	uint32_t metadata_type; /* Static_Metadata_Descriptor_ID */
	union {
		struct hdr_static_metadata hdmi_type1;
	};
};

struct drm_display_info {
	/* physical size in mm, 0 = unknown */
	unsigned int width_mm;
	unsigned int height_mm;
	/* bits per colour channel, 0 = unknown */
	unsigned int bpc;
	enum subpixel_order subpixel_order;
	/* DRM_MODE_PANEL_ORIENTATION_* (-1: unknown) */
	int panel_orientation;
	/* DRM_COLOR_FORMAT_* the sink accepts */
	uint32_t color_formats;
	/* highest TMDS clock the sink takes, kHz (0 = the EDID says none) */
	int max_tmds_clock;
	bool dvi_dual; /* dual-link DVI */
	bool is_hdmi; /* an HDMI sink (the HDMI vendor block is present) */
	bool has_audio;
	bool has_hdmi_infoframe;
	/* the sink honours the RGB quantisation range in the AVI infoframe */
	bool rgb_quant_range_selectable;
	/* deep colour depths in RGB / YCbCr 4:4:4 (DRM_EDID_HDMI_DC_*) */
	uint8_t edid_hdmi_rgb444_dc_modes;
	uint8_t edid_hdmi_ycbcr444_dc_modes;
	/* revision of the CEA extension, 0 = none */
	uint8_t cea_rev;
	struct drm_hdmi_info hdmi;
	struct hdr_sink_metadata hdr_sink_metadata;
	/* a head-mounted display or similar: not part of the desktop */
	bool non_desktop;
	struct drm_monitor_range_info monitor_range;
	struct drm_luminance_range_info luminance_range;
	/* eDP multi-SST operation: streams and pixel overlap */
	uint8_t mso_stream_count;
	uint8_t mso_pixel_overlap;
	/* highest compressed bpp (DSC), 0 = no DSC */
	uint32_t max_dsc_bpp;
	/* The CEA video codes the sink lists, in order (allocated by the
	 * EDID code, freed with the display info; NULL when none). */
	uint8_t *vics;
	int vics_len;
	/* EDID quirk bits that apply to this sink */
	uint32_t quirks;
	/* the HDMI CEC physical address, 0xffff when none */
	uint16_t source_physical_address;
};

#endif
