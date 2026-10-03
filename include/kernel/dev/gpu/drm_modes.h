// LikeOS -- display-manager core: a display mode as the core describes it.
//
// Clients see a timing as struct drm_mode_modeinfo.  Inside the core and
// the drivers a timing is a struct drm_display_mode: the same numbers, plus
// the values the hardware is actually programmed with (the crtc_* set:
// blanking spelled out, vertical values halved for interlace and doubled
// for double scan), the sink's physical size, the picture aspect ratio as
// a value of its own rather than flag bits, and whether the mode passed
// validation (status) -- with the reasons a mode can be refused.
//
// The conversions in both directions are here.  Computing names, refresh
// rates and crtc values, matching, validating, sorting and generating
// modes (CVT, GTF, analog television), and the standard timing tables
// (VESA DMT, CEA-861 video codes, HDMI 1.4 video codes) are drm_modes.c's.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Keith Packard's, Dave Airlie's and Intel's code: MIT
// Portions Copyright (C) 2006 Keith Packard
// Portions Copyright (C) 2007-2008 Dave Airlie <airlied@linux.ie>
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2014 Intel Corporation
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (C) 2016 Intel Corporation

#ifndef KERNEL_DEV_GPU_DRM_MODES_H
#define KERNEL_DEV_GPU_DRM_MODES_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/drm/drm_mode.h>

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ERANGE
#define ERANGE 34
#endif

struct drm_device;
struct drm_display_info;

/* Why a mode was refused (or MODE_OK).  The order is part of the contract:
 * code compares against MODE_OK and prints the value. */
enum drm_mode_status {
	MODE_OK = 0, /* Mode OK */
	MODE_HSYNC, /* hsync out of range */
	MODE_VSYNC, /* vsync out of range */
	MODE_H_ILLEGAL, /* mode has illegal horizontal timings */
	MODE_V_ILLEGAL, /* mode has illegal vertical timings */
	MODE_BAD_WIDTH, /* requires an unsupported linepitch */
	MODE_NOMODE, /* no mode with a matching name */
	MODE_NO_INTERLACE, /* interlaced mode not supported */
	MODE_NO_DBLESCAN, /* doublescan mode not supported */
	MODE_NO_VSCAN, /* multiscan mode not supported */
	MODE_MEM, /* insufficient video memory */
	MODE_VIRTUAL_X, /* mode width too large for specified virtual size */
	MODE_VIRTUAL_Y, /* mode height too large for specified virtual size */
	MODE_MEM_VIRT, /* insufficient video memory given virtual size */
	MODE_NOCLOCK, /* no fixed clock available */
	MODE_CLOCK_HIGH, /* clock required is too high */
	MODE_CLOCK_LOW, /* clock required is too low */
	MODE_CLOCK_RANGE, /* clock/mode isn't in a ClockRange */
	MODE_BAD_HVALUE, /* horizontal timing was out of range */
	MODE_BAD_VVALUE, /* vertical timing was out of range */
	MODE_BAD_VSCAN, /* VScan value out of range */
	MODE_HSYNC_NARROW, /* horizontal sync too narrow */
	MODE_HSYNC_WIDE, /* horizontal sync too wide */
	MODE_HBLANK_NARROW, /* horizontal blanking too narrow */
	MODE_HBLANK_WIDE, /* horizontal blanking too wide */
	MODE_VSYNC_NARROW, /* vertical sync too narrow */
	MODE_VSYNC_WIDE, /* vertical sync too wide */
	MODE_VBLANK_NARROW, /* vertical blanking too narrow */
	MODE_VBLANK_WIDE, /* vertical blanking too wide */
	MODE_PANEL, /* exceeds panel dimensions */
	MODE_INTERLACE_WIDTH, /* width too large for interlaced mode */
	MODE_ONE_WIDTH, /* only one width is supported */
	MODE_ONE_HEIGHT, /* only one height is supported */
	MODE_ONE_SIZE, /* only one resolution is supported */
	MODE_NO_REDUCED, /* monitor doesn't accept reduced blanking */
	MODE_NO_STEREO, /* stereo modes not supported */
	MODE_NO_420, /* ycbcr 420 modes not supported */
	MODE_STALE = -3, /* mode has become stale */
	MODE_BAD = -2, /* unspecified reason */
	MODE_ERROR = -1 /* error condition */
};

/* drm_mode_set_crtcinfo() adjustments. */
#define CRTC_INTERLACE_HALVE_V (1 << 0) /* halve V values for interlacing */
#define CRTC_STEREO_DOUBLE (1 << 1) /* adjust timings for stereo modes */
#define CRTC_NO_DBLSCAN (1 << 2) /* don't adjust doublescan */
#define CRTC_NO_VSCAN (1 << 3) /* don't adjust multiscan */
#define CRTC_STEREO_DOUBLE_ONLY (CRTC_STEREO_DOUBLE | CRTC_NO_DBLSCAN | CRTC_NO_VSCAN)

/* What drm_mode_match() compares. */
#define DRM_MODE_MATCH_TIMINGS (1 << 0)
#define DRM_MODE_MATCH_CLOCK (1 << 1)
#define DRM_MODE_MATCH_FLAGS (1 << 2)
#define DRM_MODE_MATCH_3D_FLAGS (1 << 3)
#define DRM_MODE_MATCH_ASPECT_RATIO (1 << 4)
#define DRM_MODE_MATCH_TIMINGS_VRR (1 << 5)

/* The highest stereo layout value a mode may carry in DRM_MODE_FLAG_3D_MASK. */
#define DRM_MODE_FLAG_3D_MAX DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF

/* A designated initialiser for a static mode table entry. */
#define DRM_MODE(nm, t, c, hd, hss, hse, ht, hsk, vd, vss, vse, vt, vs, f) \
	.name = nm, .status = 0, .type = (t), .clock = (c), \
	.hdisplay = (hd), .hsync_start = (hss), .hsync_end = (hse), \
	.htotal = (ht), .hskew = (hsk), .vdisplay = (vd), \
	.vsync_start = (vss), .vsync_end = (vse), .vtotal = (vt), \
	.vscan = (vs), .flags = (f)

/* The size in mm of `res' pixels at `dpi' dots per inch. */
#define DRM_MODE_RES_MM(res, dpi) (((res) * 254ul) / ((dpi) * 10ul))

/* A mode without blanking: for a fixed panel whose controller does the
 * timing itself.  DRM_MODE_INIT gives a clock for `hz', DRM_SIMPLE_MODE a
 * clock of 1 kHz (just enough to pass validation). */
#define __DRM_MODE_INIT(pix, hd, vd, hd_mm, vd_mm) \
	.type = DRM_MODE_TYPE_DRIVER, .clock = (pix), \
	.hdisplay = (hd), .hsync_start = (hd), .hsync_end = (hd), \
	.htotal = (hd), .vdisplay = (vd), .vsync_start = (vd), \
	.vsync_end = (vd), .vtotal = (vd), .width_mm = (hd_mm), \
	.height_mm = (vd_mm)
#define DRM_MODE_INIT(hz, hd, vd, hd_mm, vd_mm) \
	__DRM_MODE_INIT((hd) * (vd) * (hz) / 1000 /* kHz */, hd, vd, hd_mm, vd_mm)
#define DRM_SIMPLE_MODE(hd, vd, hd_mm, vd_mm) \
	__DRM_MODE_INIT(1 /* pass validation */, hd, vd, hd_mm, vd_mm)

struct drm_display_mode {
	/* The timing as the sink sees it.  Pixel clock in kHz; horizontal
	 * values in pixels, vertical values in lines; flags are
	 * DRM_MODE_FLAG_* without the picture-aspect bits (those are in
	 * picture_aspect_ratio below). */
	int clock;
	uint16_t hdisplay;
	uint16_t hsync_start;
	uint16_t hsync_end;
	uint16_t htotal;
	uint16_t hskew;
	uint16_t vdisplay;
	uint16_t vsync_start;
	uint16_t vsync_end;
	uint16_t vtotal;
	uint16_t vscan;
	uint32_t flags;

	/* The timing the hardware is programmed with, filled in by
	 * drm_mode_set_crtcinfo() (and adjusted by a driver's check): the
	 * blanking spelled out, interlace and double scan applied. */
	int crtc_clock;
	uint16_t crtc_hdisplay;
	uint16_t crtc_hblank_start;
	uint16_t crtc_hblank_end;
	uint16_t crtc_hsync_start;
	uint16_t crtc_hsync_end;
	uint16_t crtc_htotal;
	uint16_t crtc_hskew;
	uint16_t crtc_vdisplay;
	uint16_t crtc_vblank_start;
	uint16_t crtc_vblank_end;
	uint16_t crtc_vsync_start;
	uint16_t crtc_vsync_end;
	uint16_t crtc_vtotal;

	/* Physical size of the image on the sink, in mm (0 = unknown). */
	uint16_t width_mm;
	uint16_t height_mm;

	/* DRM_MODE_TYPE_*: where the mode came from (preferred, driver,
	 * user defined). */
	uint8_t type;
	/* The mode is one a client may be shown (and set). */
	bool expose_to_userspace;
	char name[DRM_DISPLAY_MODE_LEN];
	/* MODE_OK, or why validation refused the mode. */
	enum drm_mode_status status;
	/* DRM_MODE_PICTURE_ASPECT_* (the CEA-861 picture aspect values). */
	int picture_aspect_ratio;
};

/* The vertical refresh rate in Hz, rounded to the nearest: the pixel clock
 * over the frame, with interlace counting fields and double scan / multi
 * scan counting repeated lines.  0 for a mode without totals, or one whose
 * numbers overflow.  drm_mode_vrefresh() in drm_modes.c answers the same. */
static inline int drm_mode_vrefresh_calc(const struct drm_display_mode *mode)
{
	uint64_t num = 1, den = 1;

	if (mode->htotal == 0 || mode->vtotal == 0 || mode->clock <= 0)
		return 0;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (mode->vscan > 1)
		den *= mode->vscan;
	num *= (uint64_t)mode->clock;
	den *= (uint64_t)mode->htotal * mode->vtotal;
	if (num > 0xFFFFFFFFULL || den > 0xFFFFFFFFULL)
		return 0;
	return (int)((num * 1000 + den / 2) / den);
}

/* The core's mode as a client sees it: the picture aspect ratio goes back
 * into the flag bits, the refresh rate is computed. */
static inline void drm_mode_to_umode(struct drm_mode_modeinfo *out,
				     const struct drm_display_mode *in)
{
	unsigned i;

	out->clock = (uint32_t)in->clock;
	out->hdisplay = in->hdisplay;
	out->hsync_start = in->hsync_start;
	out->hsync_end = in->hsync_end;
	out->htotal = in->htotal;
	out->hskew = in->hskew;
	out->vdisplay = in->vdisplay;
	out->vsync_start = in->vsync_start;
	out->vsync_end = in->vsync_end;
	out->vtotal = in->vtotal;
	out->vscan = in->vscan;
	out->vrefresh = (uint32_t)drm_mode_vrefresh_calc(in);
	out->flags = in->flags;
	out->type = in->type;

	switch (in->picture_aspect_ratio) {
	case DRM_MODE_PICTURE_ASPECT_4_3:
		out->flags |= DRM_MODE_FLAG_PIC_AR_4_3;
		break;
	case DRM_MODE_PICTURE_ASPECT_16_9:
		out->flags |= DRM_MODE_FLAG_PIC_AR_16_9;
		break;
	case DRM_MODE_PICTURE_ASPECT_64_27:
		out->flags |= DRM_MODE_FLAG_PIC_AR_64_27;
		break;
	case DRM_MODE_PICTURE_ASPECT_256_135:
		out->flags |= DRM_MODE_FLAG_PIC_AR_256_135;
		break;
	default:
		/* an unknown value reads as "none" */
	case DRM_MODE_PICTURE_ASPECT_NONE:
		out->flags |= DRM_MODE_FLAG_PIC_AR_NONE;
		break;
	}

	for (i = 0; i + 1 < sizeof(out->name) && in->name[i]; i++)
		out->name[i] = in->name[i];
	for (; i < sizeof(out->name); i++)
		out->name[i] = 0;
}

/* A client's mode as the core's: 0, -ERANGE for a clock or rate no int
 * holds, -EINVAL for an unknown picture aspect ratio.  The type bits are
 * masked to the known ones (old clients left them uninitialised).  The
 * caller then validates the mode (drm_mode_validate_driver) and fills in
 * the crtc values (drm_mode_set_crtcinfo with CRTC_INTERLACE_HALVE_V);
 * until then status is MODE_OK and the crtc values are zero. */
static inline int drm_mode_from_umode(struct drm_display_mode *out,
				      const struct drm_mode_modeinfo *in)
{
	unsigned i;

	if (in->clock > 0x7FFFFFFFU || in->vrefresh > 0x7FFFFFFFU)
		return -ERANGE;

	for (i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
	out->clock = (int)in->clock;
	out->hdisplay = in->hdisplay;
	out->hsync_start = in->hsync_start;
	out->hsync_end = in->hsync_end;
	out->htotal = in->htotal;
	out->hskew = in->hskew;
	out->vdisplay = in->vdisplay;
	out->vsync_start = in->vsync_start;
	out->vsync_end = in->vsync_end;
	out->vtotal = in->vtotal;
	out->vscan = in->vscan;
	out->flags = in->flags;
	out->type = (uint8_t)(in->type & DRM_MODE_TYPE_ALL);
	for (i = 0; i + 1 < sizeof(out->name) && in->name[i]; i++)
		out->name[i] = in->name[i];
	out->expose_to_userspace = true;

	/* The picture aspect ratio is not kept in the flags here. */
	out->flags &= ~DRM_MODE_FLAG_PIC_AR_MASK;

	switch (in->flags & DRM_MODE_FLAG_PIC_AR_MASK) {
	case DRM_MODE_FLAG_PIC_AR_4_3:
		out->picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3;
		break;
	case DRM_MODE_FLAG_PIC_AR_16_9:
		out->picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9;
		break;
	case DRM_MODE_FLAG_PIC_AR_64_27:
		out->picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27;
		break;
	case DRM_MODE_FLAG_PIC_AR_256_135:
		out->picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135;
		break;
	case DRM_MODE_FLAG_PIC_AR_NONE:
		out->picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_NONE;
		break;
	default:
		return -EINVAL;
	}
	out->status = MODE_OK;
	return 0;
}

static inline bool drm_mode_is_stereo(const struct drm_display_mode *mode)
{
	return (mode->flags & DRM_MODE_FLAG_3D_MASK) != 0;
}

/* printf format and arguments for a mode in the log: name, refresh, clock,
 * the eight timings, type and flags. */
#define DRM_MODE_FMT "\"%s\": %d %d %d %d %d %d %d %d %d %d 0x%x 0x%x"
#define DRM_MODE_ARG(m) \
	(m)->name, drm_mode_vrefresh(m), (m)->clock, \
	(m)->hdisplay, (m)->hsync_start, (m)->hsync_end, (m)->htotal, \
	(m)->vdisplay, (m)->vsync_start, (m)->vsync_end, (m)->vtotal, \
	(m)->type, (m)->flags

/* The analog television standards a TV encoder can produce. */
#ifndef DRM_CONNECTOR_TV_MODE_DEFINED
#define DRM_CONNECTOR_TV_MODE_DEFINED
enum drm_connector_tv_mode {
	DRM_MODE_TV_MODE_NTSC, /* CCIR System M, NTSC colour */
	DRM_MODE_TV_MODE_NTSC_443, /* NTSC-like, 4.43 MHz colour subcarrier */
	DRM_MODE_TV_MODE_NTSC_J, /* the Japanese NTSC variant (no setup) */
	DRM_MODE_TV_MODE_PAL, /* CCIR System B, PAL colour */
	DRM_MODE_TV_MODE_PAL_M, /* CCIR System M, PAL colour */
	DRM_MODE_TV_MODE_PAL_N, /* CCIR System N, PAL colour */
	DRM_MODE_TV_MODE_SECAM, /* SECAM colour */
	DRM_MODE_TV_MODE_MONOCHROME, /* 625 lines, no colour */
	DRM_MODE_TV_MODE_MAX,
};
#endif

/* ---- drm_modes.c ----------------------------------------------------------
 *
 * The mode computations, on caller-provided modes: the core keeps modes in
 * arrays, not allocated lists, so a generator fills in a mode the caller
 * owns and returns 0 or -EINVAL, and the list helpers take an array and a
 * count.  The functions that work on the client form (drm_mode_cvt,
 * drm_mode_gtf, drm_mode_equal, drm_mode_finish, drm_mode_table_get,
 * drm_mode_cea_vic, drm_mode_in_range in drm_edid.h) keep their names and
 * meaning; they are built on the functions below. */

/* Name the mode "<w>x<h>" (an "i" for interlace). */
void drm_mode_set_name(struct drm_display_mode *mode);
/* Vertical refresh in Hz, rounded; see drm_mode_vrefresh_calc(). */
int drm_mode_vrefresh(const struct drm_display_mode *mode);
/* The displayed size, with stereo frame packing accounted for. */
void drm_mode_get_hv_timing(const struct drm_display_mode *mode,
			    int *hdisplay, int *vdisplay);
/* Fill in the crtc_* values; adjust_flags are CRTC_*. */
void drm_mode_set_crtcinfo(struct drm_display_mode *p, int adjust_flags);
/* Copy a mode (drm_mode_init: into a destination that may hold garbage). */
void drm_mode_copy(struct drm_display_mode *dst,
		   const struct drm_display_mode *src);
void drm_mode_init(struct drm_display_mode *dst,
		   const struct drm_display_mode *src);

/* Do the two modes agree in what match_flags (DRM_MODE_MATCH_*) names?
 * Two NULL modes agree, a NULL and a mode do not.  Clocks compare as
 * pixel periods in picoseconds, so a clock rounded differently matches. */
bool drm_mode_match(const struct drm_display_mode *mode1,
		    const struct drm_display_mode *mode2,
		    unsigned int match_flags);
/* Everything: timings, clock, flags, stereo layout, picture aspect.  (The
 * name drm_mode_equal belongs to the client-form comparison.) */
bool drm_display_mode_equal(const struct drm_display_mode *mode1,
			    const struct drm_display_mode *mode2);
bool drm_mode_equal_no_clocks(const struct drm_display_mode *mode1,
			      const struct drm_display_mode *mode2);
bool drm_mode_equal_no_clocks_no_stereo(const struct drm_display_mode *mode1,
					const struct drm_display_mode *mode2);

/* The core's checks of a mode on its own: known type and flag bits, a
 * clock, and each of the horizontal and vertical timings in order. */
enum drm_mode_status drm_mode_validate_basic(const struct drm_display_mode *mode);
/* drm_mode_validate_basic(); a driver's own limits are per connector
 * (drm_driver.mode_valid) and asked by the connector code. */
enum drm_mode_status drm_mode_validate_driver(struct drm_device *dev,
					      const struct drm_display_mode *mode);
/* MODE_VIRTUAL_X / _Y for a mode larger than maxX x maxY (0: no limit). */
enum drm_mode_status drm_mode_validate_size(const struct drm_display_mode *mode,
					    int maxX, int maxY);
/* MODE_NO_420 for a mode the sink takes only as YCbCr 4:2:0 when the
 * source cannot send that (ycbcr_420_allowed false). */
enum drm_mode_status drm_mode_validate_ycbcr420(const struct drm_display_mode *mode,
						const struct drm_display_info *info,
						bool ycbcr_420_allowed);
/* The status as text ("OK", "CLOCK_HIGH", ...), "" for an unknown value. */
const char *drm_get_mode_status_name(enum drm_mode_status status);

/* Remove from `modes' (n entries) every mode whose status is not MODE_OK,
 * keeping the order; returns the new count.  A refused user-defined mode
 * is always logged, every refused mode when `verbose'. */
int drm_mode_prune_invalid(struct drm_display_mode *modes, int n,
			   bool verbose);
/* Sort: preferred first, then larger, then higher refresh, then higher
 * clock (stable). */
void drm_mode_sort(struct drm_display_mode *modes, int n);
/* Merge freshly probed modes into a mode list: a probed mode equal to one
 * in the list (drm_display_mode_equal) replaces it when the old one is
 * MODE_STALE or the probed one is preferred and the old one is not,
 * otherwise only adds its type bits; a new mode is appended while there
 * is room (cap).  Returns the new count. */
int drm_mode_list_update(struct drm_display_mode *modes, int n, int cap,
			 const struct drm_display_mode *probed, int nprobed);
/* Mark every mode of hpref x vpref preferred. */
void drm_mode_set_preferred(struct drm_display_mode *modes, int n,
			    int hpref, int vpref);

/* YCbCr 4:2:0 capability of a mode on a sink (by its CEA video code). */
bool drm_mode_is_420_only(const struct drm_display_info *display,
			  const struct drm_display_mode *mode);
bool drm_mode_is_420_also(const struct drm_display_info *display,
			  const struct drm_display_mode *mode);
bool drm_mode_is_420(const struct drm_display_info *display,
		     const struct drm_display_mode *mode);

/* The core mode to the client form (drm_mode_to_umode) and back: 0,
 * -ERANGE, or -EINVAL for an unknown aspect ratio or a mode that fails
 * drm_mode_validate_driver() (out->status says why).  A mode that is
 * accepted has its crtc values filled in (CRTC_INTERLACE_HALVE_V). */
void drm_mode_convert_to_umode(struct drm_mode_modeinfo *out,
			       const struct drm_display_mode *in);
int drm_mode_convert_umode(struct drm_device *dev,
			   struct drm_display_mode *out,
			   const struct drm_mode_modeinfo *in);

/* Format the mode as DRM_MODE_FMT into buf (always terminated); returns
 * the length written.  drm_mode_debug_printmodeline() logs it. */
int drm_mode_snprint(char *buf, unsigned int len,
		     const struct drm_display_mode *mode);
void drm_mode_debug_printmodeline(const struct drm_display_mode *mode);

/* Timings from a geometry: CVT (reduced blanking when `reduced', borders
 * when `margins'), GTF with the default or the given formula parameters
 * (C and J doubled: pass 80 for a C of 40), and the analog television
 * timings of a standard at a pixel clock.  Each fills in *mode (zeroed
 * first) and returns 0, or -EINVAL for a geometry the formula cannot
 * produce. */
int drm_cvt_mode(struct drm_display_mode *mode, int hdisplay, int vdisplay,
		 int vrefresh, bool reduced, bool interlaced, bool margins);
int drm_gtf_mode(struct drm_display_mode *mode, int hdisplay, int vdisplay,
		 int vrefresh, bool interlaced, int margins);
int drm_gtf_mode_complex(struct drm_display_mode *mode, int hdisplay,
			 int vdisplay, int vrefresh, bool interlaced,
			 int margins, int GTF_M, int GTF_2C, int GTF_K,
			 int GTF_2J);
int drm_analog_tv_mode(struct drm_display_mode *mode,
		       enum drm_connector_tv_mode tv_mode,
		       unsigned long pixel_clock_hz, unsigned int hdisplay,
		       unsigned int vdisplay, bool interlace);

/* The two broadcast standards at the BT.601 clock. */
static inline int drm_mode_analog_ntsc_480i(struct drm_display_mode *mode)
{
	return drm_analog_tv_mode(mode, DRM_MODE_TV_MODE_NTSC, 13500000, 720,
				  480, true);
}

static inline int drm_mode_analog_pal_576i(struct drm_display_mode *mode)
{
	return drm_analog_tv_mode(mode, DRM_MODE_TV_MODE_PAL, 13500000, 720,
				  576, true);
}

/* ---- the standard timing tables (drm_modes.c) ------------------------------
 *
 * One copy of each table, for the EDID code and the drivers alike.  The
 * entries are const; copy one with drm_mode_init() to change it. */

/* VESA DMT: entry i (0 .. drm_dmt_num_modes() - 1), NULL past the end. */
int drm_dmt_num_modes(void);
const struct drm_display_mode *drm_dmt_mode(int i);
/* The DMT entry of hsize x vsize at `fresh' Hz, reduced blanking or not;
 * NULL when DMT has none. */
const struct drm_display_mode *drm_mode_find_dmt(int hsize, int vsize,
						 int fresh, bool rb);
/* Does the mode have CVT reduced-blanking (v1) timing? */
bool drm_mode_is_rb(const struct drm_display_mode *mode);

/* CEA-861 video codes 1-127 and 193-219.  Walk them with
 * for (vic = 1; vic < drm_cea_num_vics(); vic = drm_cea_next_vic(vic)). */
const struct drm_display_mode *drm_cea_mode_for_vic(uint8_t vic);
uint8_t drm_cea_num_vics(void);
uint8_t drm_cea_next_vic(uint8_t vic);
bool drm_valid_cea_vic(uint8_t vic);
/* DRM_MODE_PICTURE_ASPECT_* of a video code (NONE when unknown). */
int drm_get_cea_aspect_ratio(uint8_t vic);
/* The same timing at the other of 60 / 59.94 Hz (and so on): the clock
 * scaled by 1000/1001 or back.  A code whose refresh is not a multiple
 * of 6 has none (its own clock is returned). */
unsigned int drm_cea_mode_alternate_clock(const struct drm_display_mode *cea_mode);
/* The codes whose vertical front porch may be a line or two longer: step
 * `mode' (a copy of the code's entry) to the next variant; false when
 * there is none left. */
bool drm_cea_mode_alternate_timings(uint8_t vic, struct drm_display_mode *mode);
/* The video code of a mode (0: none): timings and flags must match, the
 * picture aspect too when the mode has one; the clock may be either
 * variant, exactly (as a pixel period) or within clock_tolerance kHz. */
uint8_t drm_match_cea_mode(const struct drm_display_mode *to_match);
uint8_t drm_match_cea_mode_clock_tolerance(const struct drm_display_mode *to_match,
					   unsigned int clock_tolerance);

/* HDMI 1.4 video codes (the 4k modes of the HDMI vendor block), 1-4. */
const struct drm_display_mode *drm_hdmi_mode_for_vic(uint8_t vic);
int drm_hdmi_num_vics(void); /* one past the highest code */
bool drm_valid_hdmi_vic(uint8_t vic);
int drm_get_hdmi_aspect_ratio(uint8_t vic);
uint8_t drm_match_hdmi_mode(const struct drm_display_mode *to_match);
uint8_t drm_match_hdmi_mode_clock_tolerance(const struct drm_display_mode *to_match,
					    unsigned int clock_tolerance);

#endif
