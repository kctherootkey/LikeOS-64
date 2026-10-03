// LikeOS -- the EDID parser: the modes a sink lists and what it can do.
//
// Modes come from every place an EDID keeps them, in the order the EDID
// ranks them: detailed timings (the first one usually the preferred
// mode), CVT 3-byte codes, standard timings (DMT, else GTF, secondary GTF
// or CVT as the EDID's version and range descriptor say), established
// timings I-III, the CTA-861 video data blocks (with the HDMI 4K codes,
// the stereo modes the HDMI block announces and the 4:2:0-only block),
// the 59.94/60 Hz twins of CTA modes, DisplayID detailed and formula
// timings, and modes inferred from a continuous-frequency range
// descriptor.  The quirk list corrects sinks known to lie.
//
// The sink's facts go into struct drm_display_info: size, colour depth
// and formats, HDMI (deep colour, TMDS clock, SCDC/scrambling, FRL, DSC,
// 4:2:0 maps), HDR static metadata and luminance, the VRR refresh range,
// non-desktop use, eDP MSO, the CTA video codes; plus the ELD, latency
// and DisplayID tile data of the connector.
//
// drm_edid_parse() keeps the drivers' fixed-size struct drm_edid_info on
// top of all that.
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

#define LEVEL_DMT 0
#define LEVEL_GTF 1
#define LEVEL_GTF2 2
#define LEVEL_CVT 3

struct detailed_mode_closure {
	struct drm_edid_conn *conn;
	const struct drm_edid *drm_edid;
	bool preferred;
	int modes;
};

static int edid_add_mode(struct drm_edid_conn *conn, const struct drm_display_mode *mode)
{
	drm_edid_probed_add(conn, mode);
	return 1;
}

#define MODE_SIZE(m) ((m)->hdisplay * (m)->vdisplay)
#define MODE_REFRESH_DIFF(c, t) (edid_abs((c) - (t)))

/*
 * Walk the modes added since `first', clearing the preferred status on
 * them and setting it anew for the right mode ala quirks: the largest,
 * closest to the quirk's refresh rate.
 */
static void edid_fixup_preferred(struct drm_edid_conn *conn, int first)
{
	struct drm_display_mode *cur_mode, *preferred_mode;
	int target_refresh = 0;
	int cur_vrefresh, preferred_vrefresh;
	int i;

	if (first >= conn->nmodes)
		return;

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_PREFER_LARGE_60))
		target_refresh = 60;
	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_PREFER_LARGE_75))
		target_refresh = 75;

	preferred_mode = &conn->modes[first];

	for (i = first; i < conn->nmodes; i++) {
		cur_mode = &conn->modes[i];
		cur_mode->type &= (uint8_t)~DRM_MODE_TYPE_PREFERRED;

		if (cur_mode == preferred_mode)
			continue;

		/* Largest mode is preferred */
		if (MODE_SIZE(cur_mode) > MODE_SIZE(preferred_mode))
			preferred_mode = cur_mode;

		cur_vrefresh = drm_mode_vrefresh(cur_mode);
		preferred_vrefresh = drm_mode_vrefresh(preferred_mode);
		/* At a given size, try to get closest to target refresh */
		if ((MODE_SIZE(cur_mode) == MODE_SIZE(preferred_mode)) &&
		    MODE_REFRESH_DIFF(cur_vrefresh, target_refresh) <
			    MODE_REFRESH_DIFF(preferred_vrefresh, target_refresh)) {
			preferred_mode = cur_mode;
		}
	}

	preferred_mode->type |= DRM_MODE_TYPE_PREFERRED;
}

/* The DMT entry, copied into `out'; false when DMT has none. */
static bool edid_find_dmt(int hsize, int vsize, int fresh, bool rb,
			  struct drm_display_mode *out)
{
	const struct drm_display_mode *ptr = drm_mode_find_dmt(hsize, vsize, fresh, rb);

	if (!ptr)
		return false;
	*out = *ptr;
	return true;
}

static void is_rb(const struct detailed_timing *descriptor, void *data)
{
	bool *res = data;

	if (!is_display_descriptor(descriptor, EDID_DETAIL_MONITOR_RANGE))
		return;

	if (descriptor->data.other_data.data.range.flags == DRM_EDID_CVT_SUPPORT_FLAG &&
	    descriptor->data.other_data.data.range.formula.cvt.flags & DRM_EDID_CVT_FLAGS_REDUCED_BLANKING)
		*res = true;
}

/* EDID 1.4 defines this explicitly.  For EDID 1.3, we guess, badly. */
static bool drm_monitor_supports_rb(const struct drm_edid *drm_edid)
{
	if (drm_edid->edid->revision >= 4) {
		bool ret = false;

		drm_for_each_detailed_block(drm_edid, is_rb, &ret);
		return ret;
	}

	return drm_edid_is_digital(drm_edid);
}

static void find_gtf2(const struct detailed_timing *descriptor, void *data)
{
	const struct detailed_timing **res = data;

	if (!is_display_descriptor(descriptor, EDID_DETAIL_MONITOR_RANGE))
		return;

	if (descriptor->data.other_data.data.range.flags == DRM_EDID_SECONDARY_GTF_SUPPORT_FLAG)
		*res = descriptor;
}

/* Secondary GTF curve kicks in above some break frequency */
static int drm_gtf2_hbreak(const struct drm_edid *drm_edid)
{
	const struct detailed_timing *descriptor = NULL;

	drm_for_each_detailed_block(drm_edid, find_gtf2, &descriptor);

	return descriptor ? descriptor->data.other_data.data.range.formula.gtf2.hfreq_start_khz * 2 : 0;
}

static int drm_gtf2_2c(const struct drm_edid *drm_edid)
{
	const struct detailed_timing *descriptor = NULL;

	drm_for_each_detailed_block(drm_edid, find_gtf2, &descriptor);

	return descriptor ? descriptor->data.other_data.data.range.formula.gtf2.c : 0;
}

static int drm_gtf2_m(const struct drm_edid *drm_edid)
{
	const struct detailed_timing *descriptor = NULL;

	drm_for_each_detailed_block(drm_edid, find_gtf2, &descriptor);

	return descriptor ? descriptor->data.other_data.data.range.formula.gtf2.m : 0;
}

static int drm_gtf2_k(const struct drm_edid *drm_edid)
{
	const struct detailed_timing *descriptor = NULL;

	drm_for_each_detailed_block(drm_edid, find_gtf2, &descriptor);

	return descriptor ? descriptor->data.other_data.data.range.formula.gtf2.k : 0;
}

static int drm_gtf2_2j(const struct drm_edid *drm_edid)
{
	const struct detailed_timing *descriptor = NULL;

	drm_for_each_detailed_block(drm_edid, find_gtf2, &descriptor);

	return descriptor ? descriptor->data.other_data.data.range.formula.gtf2.j : 0;
}

static void get_timing_level(const struct detailed_timing *descriptor, void *data)
{
	int *res = data;

	if (!is_display_descriptor(descriptor, EDID_DETAIL_MONITOR_RANGE))
		return;

	switch (descriptor->data.other_data.data.range.flags) {
	case DRM_EDID_DEFAULT_GTF_SUPPORT_FLAG:
		*res = LEVEL_GTF;
		break;
	case DRM_EDID_SECONDARY_GTF_SUPPORT_FLAG:
		*res = LEVEL_GTF2;
		break;
	case DRM_EDID_CVT_SUPPORT_FLAG:
		*res = LEVEL_CVT;
		break;
	default:
		break;
	}
}

/* Get standard timing level (CVT/GTF/DMT). */
static int standard_timing_level(const struct drm_edid *drm_edid)
{
	const struct edid *edid = drm_edid->edid;

	if (edid->revision >= 4) {
		/*
		 * If the range descriptor doesn't
		 * indicate otherwise default to CVT
		 */
		int ret = LEVEL_CVT;

		drm_for_each_detailed_block(drm_edid, get_timing_level, &ret);

		return ret;
	} else if (edid->revision >= 3 && drm_gtf2_hbreak(drm_edid)) {
		return LEVEL_GTF2;
	} else if (edid->revision >= 2) {
		return LEVEL_GTF;
	} else {
		return LEVEL_DMT;
	}
}

/*
 * 0 is reserved.  The spec says 0x01 fill for unused timings.  Some old
 * monitors fill with ascii space (0x20) instead.
 */
static int bad_std_timing(uint8_t a, uint8_t b)
{
	return (a == 0x00 && b == 0x00) ||
	       (a == 0x01 && b == 0x01) ||
	       (a == 0x20 && b == 0x20);
}

static int drm_mode_hsync(const struct drm_display_mode *mode)
{
	if (mode->htotal <= 0)
		return 0;

	return DIV_ROUND_CLOSEST(mode->clock, mode->htotal);
}

static bool drm_gtf2_mode(const struct drm_edid *drm_edid,
			  int hsize, int vsize, int vrefresh_rate,
			  struct drm_display_mode *mode)
{
	/*
	 * This is potentially wrong if there's ever a monitor with
	 * more than one ranges section, each claiming a different
	 * secondary GTF curve.  Please don't do that.
	 */
	if (drm_gtf_mode(mode, hsize, vsize, vrefresh_rate, 0, 0))
		return false;

	if (drm_mode_hsync(mode) > drm_gtf2_hbreak(drm_edid)) {
		if (drm_gtf_mode_complex(mode, hsize, vsize,
					 vrefresh_rate, 0, 0,
					 drm_gtf2_m(drm_edid),
					 drm_gtf2_2c(drm_edid),
					 drm_gtf2_k(drm_edid),
					 drm_gtf2_2j(drm_edid)))
			return false;
	}

	return true;
}

/*
 * Take the standard timing params (in this case width, aspect, and refresh)
 * and convert them into a real mode using CVT/GTF/DMT.
 */
static bool drm_mode_std(struct drm_edid_conn *conn,
			 const struct drm_edid *drm_edid,
			 const struct std_timing *t,
			 struct drm_display_mode *mode)
{
	const struct drm_display_mode *m;
	int hsize, vsize;
	int vrefresh_rate;
	unsigned aspect_ratio = (t->vfreq_aspect & EDID_TIMING_ASPECT_MASK)
		>> EDID_TIMING_ASPECT_SHIFT;
	unsigned vfreq = (t->vfreq_aspect & EDID_TIMING_VFREQ_MASK)
		>> EDID_TIMING_VFREQ_SHIFT;
	int timing_level = standard_timing_level(drm_edid);
	int i;

	if (bad_std_timing(t->hsize, t->vfreq_aspect))
		return false;

	/* According to the EDID spec, the hdisplay = hsize * 8 + 248 */
	hsize = t->hsize * 8 + 248;
	/* vrefresh_rate = vfreq + 60 */
	vrefresh_rate = (int)vfreq + 60;
	/* the vdisplay is calculated based on the aspect ratio */
	if (aspect_ratio == 0) {
		if (drm_edid->edid->revision < 3)
			vsize = hsize;
		else
			vsize = (hsize * 10) / 16;
	} else if (aspect_ratio == 1)
		vsize = (hsize * 3) / 4;
	else if (aspect_ratio == 2)
		vsize = (hsize * 4) / 5;
	else
		vsize = (hsize * 9) / 16;

	/* HDTV hack, part 1 */
	if (vrefresh_rate == 60 &&
	    ((hsize == 1360 && vsize == 765) ||
	     (hsize == 1368 && vsize == 769))) {
		hsize = 1366;
		vsize = 768;
	}

	/*
	 * If this connector already has a mode for this size and refresh
	 * rate (because it came from detailed or CVT info), use that
	 * instead.  This way we don't have to guess at interlace or
	 * reduced blanking.
	 */
	for (i = 0; i < conn->nmodes; i++) {
		m = &conn->modes[i];
		if (m->hdisplay == hsize && m->vdisplay == vsize &&
		    drm_mode_vrefresh(m) == vrefresh_rate)
			return false;
	}

	/* HDTV hack, part 2 */
	if (hsize == 1366 && vsize == 768 && vrefresh_rate == 60) {
		if (drm_cvt_mode(mode, 1366, 768, vrefresh_rate, 0, 0, false))
			return false;
		mode->hdisplay = 1366;
		mode->hsync_start = mode->hsync_start - 1;
		mode->hsync_end = mode->hsync_end - 1;
		return true;
	}

	/* check whether it can be found in default mode table */
	if (drm_monitor_supports_rb(drm_edid)) {
		if (edid_find_dmt(hsize, vsize, vrefresh_rate, true, mode))
			return true;
	}
	if (edid_find_dmt(hsize, vsize, vrefresh_rate, false, mode))
		return true;

	/* okay, generate it */
	switch (timing_level) {
	case LEVEL_DMT:
		break;
	case LEVEL_GTF:
		return drm_gtf_mode(mode, hsize, vsize, vrefresh_rate, 0, 0) == 0;
	case LEVEL_GTF2:
		return drm_gtf2_mode(drm_edid, hsize, vsize, vrefresh_rate, mode);
	case LEVEL_CVT:
		return drm_cvt_mode(mode, hsize, vsize, vrefresh_rate, 0, 0, false) == 0;
	}
	return false;
}

/*
 * EDID is delightfully ambiguous about how interlaced modes are to be
 * encoded.  Our internal representation is of frame height, but some
 * HDTV detailed timings are encoded as field height.
 *
 * The format list here is from CEA, in frame size.  Technically we
 * should be checking refresh rate too.  Whatever.
 */
static void drm_mode_do_interlace_quirk(struct drm_display_mode *mode,
					const struct detailed_pixel_timing *pt)
{
	unsigned int i;
	static const struct {
		int w, h;
	} cea_interlaced[] = {
		{ 1920, 1080 },
		{ 720, 480 },
		{ 1440, 480 },
		{ 2880, 480 },
		{ 720, 576 },
		{ 1440, 576 },
		{ 2880, 576 },
	};

	if (!(pt->misc & DRM_EDID_PT_INTERLACED))
		return;

	for (i = 0; i < ARRAY_SIZE(cea_interlaced); i++) {
		if ((mode->hdisplay == cea_interlaced[i].w) &&
		    (mode->vdisplay == cea_interlaced[i].h / 2)) {
			mode->vdisplay *= 2;
			mode->vsync_start *= 2;
			mode->vsync_end *= 2;
			mode->vtotal *= 2;
			mode->vtotal |= 1;
		}
	}

	mode->flags |= DRM_MODE_FLAG_INTERLACE;
}

/*
 * Create a new mode from an EDID detailed timing section. An EDID detailed
 * timing block contains enough info for us to create a new struct
 * drm_display_mode.
 */
static bool drm_mode_detailed(struct drm_edid_conn *conn,
			      const struct drm_edid *drm_edid,
			      const struct detailed_timing *timing,
			      struct drm_display_mode *mode)
{
	const struct detailed_pixel_timing *pt = &timing->data.pixel_data;
	unsigned hactive = (pt->hactive_hblank_hi & 0xf0) << 4 | pt->hactive_lo;
	unsigned vactive = (pt->vactive_vblank_hi & 0xf0) << 4 | pt->vactive_lo;
	unsigned hblank = (pt->hactive_hblank_hi & 0xf) << 8 | pt->hblank_lo;
	unsigned vblank = (pt->vactive_vblank_hi & 0xf) << 8 | pt->vblank_lo;
	unsigned hsync_offset = (pt->hsync_vsync_offset_pulse_width_hi & 0xc0) << 2 | pt->hsync_offset_lo;
	unsigned hsync_pulse_width = (pt->hsync_vsync_offset_pulse_width_hi & 0x30) << 4 | pt->hsync_pulse_width_lo;
	unsigned vsync_offset = (pt->hsync_vsync_offset_pulse_width_hi & 0xc) << 2 | pt->vsync_offset_pulse_width_lo >> 4;
	unsigned vsync_pulse_width = (pt->hsync_vsync_offset_pulse_width_hi & 0x3) << 4 | (pt->vsync_offset_pulse_width_lo & 0xf);

	/* ignore tiny modes */
	if (hactive < 64 || vactive < 64)
		return false;

	/* Stereo modes are not supported. */
	if (pt->misc & DRM_EDID_PT_STEREO)
		return false;
	/* Composite sync is not supported either, but the timing is kept:
	 * the sync polarity bits below are then meaningless. */

	/* it is incorrect if hsync/vsync width is zero */
	if (!hsync_pulse_width || !vsync_pulse_width)
		return false;

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_FORCE_REDUCED_BLANKING)) {
		if (drm_cvt_mode(mode, (int)hactive, (int)vactive, 60, true, false, false))
			return false;

		goto set_size;
	}

	edid_memset(mode, 0, sizeof(*mode));

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_135_CLOCK_TOO_HIGH))
		mode->clock = 1088 * 10;
	else
		mode->clock = timing->pixel_clock * 10;

	mode->hdisplay = (uint16_t)hactive;
	mode->hsync_start = (uint16_t)(mode->hdisplay + hsync_offset);
	mode->hsync_end = (uint16_t)(mode->hsync_start + hsync_pulse_width);
	mode->htotal = (uint16_t)(mode->hdisplay + hblank);

	mode->vdisplay = (uint16_t)vactive;
	mode->vsync_start = (uint16_t)(mode->vdisplay + vsync_offset);
	mode->vsync_end = (uint16_t)(mode->vsync_start + vsync_pulse_width);
	mode->vtotal = (uint16_t)(mode->vdisplay + vblank);

	/* Some EDIDs have bogus h/vsync_end values */
	if (mode->hsync_end > mode->htotal)
		mode->hsync_end = mode->htotal;
	if (mode->vsync_end > mode->vtotal)
		mode->vsync_end = mode->vtotal;

	drm_mode_do_interlace_quirk(mode, pt);

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_DETAILED_SYNC_PP)) {
		mode->flags |= DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
	} else {
		mode->flags |= (pt->misc & DRM_EDID_PT_HSYNC_POSITIVE) ?
			DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
		mode->flags |= (pt->misc & DRM_EDID_PT_VSYNC_POSITIVE) ?
			DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
	}

set_size:
	mode->width_mm = (uint16_t)(pt->width_mm_lo | (pt->width_height_mm_hi & 0xf0) << 4);
	mode->height_mm = (uint16_t)(pt->height_mm_lo | (pt->width_height_mm_hi & 0xf) << 8);

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_DETAILED_IN_CM)) {
		mode->width_mm *= 10;
		mode->height_mm *= 10;
	}

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_DETAILED_USE_MAXIMUM_SIZE)) {
		mode->width_mm = (uint16_t)(drm_edid->edid->width_cm * 10);
		mode->height_mm = (uint16_t)(drm_edid->edid->height_cm * 10);
	}

	mode->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(mode);

	return true;
}

static bool mode_in_hsync_range(const struct drm_display_mode *mode,
				const struct edid *edid, const uint8_t *t)
{
	int hsync, hmin, hmax;

	hmin = t[7];
	if (edid->revision >= 4)
		hmin += ((t[4] & 0x04) ? 255 : 0);
	hmax = t[8];
	if (edid->revision >= 4)
		hmax += ((t[4] & 0x08) ? 255 : 0);
	hsync = drm_mode_hsync(mode);

	return (hsync <= hmax && hsync >= hmin);
}

static bool mode_in_vsync_range(const struct drm_display_mode *mode,
				const struct edid *edid, const uint8_t *t)
{
	int vsync, vmin, vmax;

	vmin = t[5];
	if (edid->revision >= 4)
		vmin += ((t[4] & 0x01) ? 255 : 0);
	vmax = t[6];
	if (edid->revision >= 4)
		vmax += ((t[4] & 0x02) ? 255 : 0);
	vsync = drm_mode_vrefresh(mode);

	return (vsync <= vmax && vsync >= vmin);
}

static uint32_t range_pixel_clock(const struct edid *edid, const uint8_t *t)
{
	/* unspecified */
	if (t[9] == 0 || t[9] == 255)
		return 0;

	/* 1.4 with CVT support gives us real precision, yay */
	if (edid->revision >= 4 && t[10] == DRM_EDID_CVT_SUPPORT_FLAG)
		return (uint32_t)((t[9] * 10000) - ((t[12] >> 2) * 250));

	/* 1.3 is pathetic, so fuzz up a bit */
	return (uint32_t)(t[9] * 10000 + 5001);
}

static bool mode_in_range(const struct drm_display_mode *mode,
			  const struct drm_edid *drm_edid,
			  const struct detailed_timing *timing)
{
	const struct edid *edid = drm_edid->edid;
	uint32_t max_clock;
	const uint8_t *t = (const uint8_t *)timing;

	if (!mode_in_hsync_range(mode, edid, t))
		return false;

	if (!mode_in_vsync_range(mode, edid, t))
		return false;

	max_clock = range_pixel_clock(edid, t);
	if (max_clock)
		if ((uint32_t)mode->clock > max_clock)
			return false;

	/* 1.4 max horizontal check */
	if (edid->revision >= 4 && t[10] == DRM_EDID_CVT_SUPPORT_FLAG)
		if (t[13] && mode->hdisplay > 8 * (t[13] + (256 * (t[12] & 0x3))))
			return false;

	if (drm_mode_is_rb(mode) && !drm_monitor_supports_rb(drm_edid))
		return false;

	return true;
}

static bool valid_inferred_mode(const struct drm_edid_conn *conn,
				const struct drm_display_mode *mode)
{
	const struct drm_display_mode *m;
	bool ok = false;
	int i;

	for (i = 0; i < conn->nmodes; i++) {
		m = &conn->modes[i];
		if (mode->hdisplay == m->hdisplay &&
		    mode->vdisplay == m->vdisplay &&
		    drm_mode_vrefresh(mode) == drm_mode_vrefresh(m))
			return false; /* duplicated */
		if (mode->hdisplay <= m->hdisplay &&
		    mode->vdisplay <= m->vdisplay)
			ok = true;
	}
	return ok;
}

static int drm_dmt_modes_for_range(struct drm_edid_conn *conn,
				   const struct drm_edid *drm_edid,
				   const struct detailed_timing *timing)
{
	int i, modes = 0;

	for (i = 0; i < drm_dmt_num_modes(); i++) {
		const struct drm_display_mode *dmt = drm_dmt_mode(i);

		if (mode_in_range(dmt, drm_edid, timing) &&
		    valid_inferred_mode(conn, dmt))
			modes += edid_add_mode(conn, dmt);
	}

	return modes;
}

/* fix up 1366x768 mode from 1368x768;
 * GFT/CVT can't express 1366 width which isn't dividable by 8
 */
static void edid_mode_fixup_1366x768(struct drm_display_mode *mode)
{
	if (mode->hdisplay == 1368 && mode->vdisplay == 768) {
		mode->hdisplay = 1366;
		mode->hsync_start--;
		mode->hsync_end--;
		drm_mode_set_name(mode);
	}
}

static int drm_gtf_modes_for_range(struct drm_edid_conn *conn,
				   const struct drm_edid *drm_edid,
				   const struct detailed_timing *timing)
{
	int i, modes = 0;
	struct drm_display_mode newmode;

	for (i = 0; i < edid_tbl_extra_count; i++) {
		const struct minimode *m = &edid_tbl_extra_modes[i];

		if (drm_gtf_mode(&newmode, m->w, m->h, m->r, 0, 0))
			return modes;

		edid_mode_fixup_1366x768(&newmode);
		if (!mode_in_range(&newmode, drm_edid, timing) ||
		    !valid_inferred_mode(conn, &newmode))
			continue;

		modes += edid_add_mode(conn, &newmode);
	}

	return modes;
}

static int drm_gtf2_modes_for_range(struct drm_edid_conn *conn,
				    const struct drm_edid *drm_edid,
				    const struct detailed_timing *timing)
{
	int i, modes = 0;
	struct drm_display_mode newmode;

	for (i = 0; i < edid_tbl_extra_count; i++) {
		const struct minimode *m = &edid_tbl_extra_modes[i];

		if (!drm_gtf2_mode(drm_edid, m->w, m->h, m->r, &newmode))
			return modes;

		edid_mode_fixup_1366x768(&newmode);
		if (!mode_in_range(&newmode, drm_edid, timing) ||
		    !valid_inferred_mode(conn, &newmode))
			continue;

		modes += edid_add_mode(conn, &newmode);
	}

	return modes;
}

static int drm_cvt_modes_for_range(struct drm_edid_conn *conn,
				   const struct drm_edid *drm_edid,
				   const struct detailed_timing *timing)
{
	int i, modes = 0;
	struct drm_display_mode newmode;
	bool rb = drm_monitor_supports_rb(drm_edid);

	for (i = 0; i < edid_tbl_extra_count; i++) {
		const struct minimode *m = &edid_tbl_extra_modes[i];

		if (drm_cvt_mode(&newmode, m->w, m->h, m->r, rb, 0, 0))
			return modes;

		edid_mode_fixup_1366x768(&newmode);
		if (!mode_in_range(&newmode, drm_edid, timing) ||
		    !valid_inferred_mode(conn, &newmode))
			continue;

		modes += edid_add_mode(conn, &newmode);
	}

	return modes;
}

static void do_inferred_modes(const struct detailed_timing *timing, void *c)
{
	struct detailed_mode_closure *closure = c;
	const struct detailed_non_pixel *data = &timing->data.other_data;
	const struct detailed_data_monitor_range *range = &data->data.range;

	if (!is_display_descriptor(timing, EDID_DETAIL_MONITOR_RANGE))
		return;

	closure->modes += drm_dmt_modes_for_range(closure->conn,
						  closure->drm_edid,
						  timing);

	if (closure->drm_edid->edid->revision < 2)
		return; /* GTF not defined yet */

	switch (range->flags) {
	case DRM_EDID_SECONDARY_GTF_SUPPORT_FLAG:
		closure->modes += drm_gtf2_modes_for_range(closure->conn,
							   closure->drm_edid,
							   timing);
		break;
	case DRM_EDID_DEFAULT_GTF_SUPPORT_FLAG:
		closure->modes += drm_gtf_modes_for_range(closure->conn,
							  closure->drm_edid,
							  timing);
		break;
	case DRM_EDID_CVT_SUPPORT_FLAG:
		if (closure->drm_edid->edid->revision < 4)
			break;

		closure->modes += drm_cvt_modes_for_range(closure->conn,
							  closure->drm_edid,
							  timing);
		break;
	case DRM_EDID_RANGE_LIMITS_ONLY_FLAG:
	default:
		break;
	}
}

static int add_inferred_modes(struct drm_edid_conn *conn,
			      const struct drm_edid *drm_edid)
{
	struct detailed_mode_closure closure = {
		.conn = conn,
		.drm_edid = drm_edid,
	};

	if (drm_edid->edid->revision >= 1)
		drm_for_each_detailed_block(drm_edid, do_inferred_modes, &closure);

	return closure.modes;
}

static int drm_est3_modes(struct drm_edid_conn *conn, const struct detailed_timing *timing)
{
	int i, j, m, modes = 0;
	struct drm_display_mode mode;
	const uint8_t *est = ((const uint8_t *)timing) + 6;

	for (i = 0; i < 6; i++) {
		for (j = 7; j >= 0; j--) {
			m = (i * 8) + (7 - j);
			if (m >= edid_tbl_est3_count)
				break;
			if (est[i] & (1 << j)) {
				if (edid_find_dmt(edid_tbl_est3_modes[m].w,
						  edid_tbl_est3_modes[m].h,
						  edid_tbl_est3_modes[m].r,
						  edid_tbl_est3_modes[m].rb, &mode))
					modes += edid_add_mode(conn, &mode);
			}
		}
	}

	return modes;
}

static void do_established_modes(const struct detailed_timing *timing, void *c)
{
	struct detailed_mode_closure *closure = c;

	if (!is_display_descriptor(timing, EDID_DETAIL_EST_TIMINGS))
		return;

	closure->modes += drm_est3_modes(closure->conn, timing);
}

/*
 * Get established modes from EDID and add them. Each EDID block contains a
 * bitmap of the supported "established modes" list (defined above). Tease them
 * out and add them to the global modes list.
 */
static int add_established_modes(struct drm_edid_conn *conn,
				 const struct drm_edid *drm_edid)
{
	const struct edid *edid = drm_edid->edid;
	unsigned long est_bits = edid->established_timings.t1 |
		(edid->established_timings.t2 << 8) |
		((edid->established_timings.mfg_rsvd & 0x80) << 9);
	int i, modes = 0;
	struct detailed_mode_closure closure = {
		.conn = conn,
		.drm_edid = drm_edid,
	};

	for (i = 0; i <= EDID_EST_TIMINGS; i++) {
		if (est_bits & (1 << i))
			modes += edid_add_mode(conn, &edid_tbl_est_modes[i]);
	}

	if (edid->revision >= 1)
		drm_for_each_detailed_block(drm_edid, do_established_modes,
					    &closure);

	return modes + closure.modes;
}

static void do_standard_modes(const struct detailed_timing *timing, void *c)
{
	struct detailed_mode_closure *closure = c;
	const struct detailed_non_pixel *data = &timing->data.other_data;
	struct drm_edid_conn *conn = closure->conn;
	struct drm_display_mode newmode;
	int i;

	if (!is_display_descriptor(timing, EDID_DETAIL_STD_MODES))
		return;

	for (i = 0; i < 6; i++) {
		const struct std_timing *std = &data->data.timings[i];

		if (drm_mode_std(conn, closure->drm_edid, std, &newmode))
			closure->modes += edid_add_mode(conn, &newmode);
	}
}

/*
 * Get standard modes from EDID and add them. Standard modes can be calculated
 * using the appropriate standard (DMT, GTF, or CVT). Grab them from EDID and
 * add them to the list.
 */
static int add_standard_modes(struct drm_edid_conn *conn,
			      const struct drm_edid *drm_edid)
{
	int i, modes = 0;
	struct drm_display_mode newmode;
	struct detailed_mode_closure closure = {
		.conn = conn,
		.drm_edid = drm_edid,
	};

	for (i = 0; i < EDID_STD_TIMINGS; i++) {
		if (drm_mode_std(conn, drm_edid,
				 &drm_edid->edid->standard_timings[i], &newmode))
			modes += edid_add_mode(conn, &newmode);
	}

	if (drm_edid->edid->revision >= 1)
		drm_for_each_detailed_block(drm_edid, do_standard_modes,
					    &closure);

	/* XXX should also look for standard codes in VTB blocks */

	return modes + closure.modes;
}

static int drm_cvt_modes(struct drm_edid_conn *conn,
			 const struct detailed_timing *timing)
{
	int i, j, modes = 0;
	struct drm_display_mode newmode;
	const struct cvt_timing *cvt;
	static const int rates[] = { 60, 85, 75, 60, 50 };
	const uint8_t empty[3] = { 0, 0, 0 };

	for (i = 0; i < 4; i++) {
		int width, height;

		cvt = &(timing->data.other_data.data.cvt[i]);

		if (!edid_memcmp(cvt->code, empty, 3))
			continue;

		height = (cvt->code[0] + ((cvt->code[1] & 0xf0) << 4) + 1) * 2;
		switch (cvt->code[1] & 0x0c) {
		/* default - because compiler doesn't see that we've enumerated all cases */
		default:
		case 0x00:
			width = height * 4 / 3;
			break;
		case 0x04:
			width = height * 16 / 9;
			break;
		case 0x08:
			width = height * 16 / 10;
			break;
		case 0x0c:
			width = height * 15 / 9;
			break;
		}

		for (j = 1; j < 5; j++) {
			if (cvt->code[2] & (1 << j)) {
				if (!drm_cvt_mode(&newmode, width, height,
						  rates[j], j == 0,
						  false, false))
					modes += edid_add_mode(conn, &newmode);
			}
		}
	}

	return modes;
}

static void do_cvt_mode(const struct detailed_timing *timing, void *c)
{
	struct detailed_mode_closure *closure = c;

	if (!is_display_descriptor(timing, EDID_DETAIL_CVT_3BYTE))
		return;

	closure->modes += drm_cvt_modes(closure->conn, timing);
}

static int add_cvt_modes(struct drm_edid_conn *conn, const struct drm_edid *drm_edid)
{
	struct detailed_mode_closure closure = {
		.conn = conn,
		.drm_edid = drm_edid,
	};

	if (drm_edid->edid->revision >= 3)
		drm_for_each_detailed_block(drm_edid, do_cvt_mode, &closure);

	/* XXX should also look for CVT codes in VTB blocks */

	return closure.modes;
}

/*
 * Detailed modes are limited to 10kHz pixel clock resolution, so a mode
 * that looks like a CEA/HDMI mode but whose clock is just slightly off
 * gets the exact clock: allow 5kHz difference either way, pick whichever
 * of the 60 and 59.94 Hz variants is closest.
 */
static void fixup_detailed_cea_mode_clock(struct drm_display_mode *mode)
{
	const struct drm_display_mode *cea_mode;
	int clock1, clock2, clock;
	uint8_t vic;

	vic = drm_match_cea_mode_clock_tolerance(mode, 5);
	if (drm_valid_cea_vic(vic)) {
		cea_mode = drm_cea_mode_for_vic(vic);
		clock1 = cea_mode->clock;
		clock2 = (int)drm_cea_mode_alternate_clock(cea_mode);
	} else {
		vic = drm_match_hdmi_mode_clock_tolerance(mode, 5);
		if (drm_valid_hdmi_vic(vic)) {
			cea_mode = drm_hdmi_mode_for_vic(vic);
			clock1 = cea_mode->clock;
			clock2 = (int)drm_cea_mode_alternate_clock(cea_mode);
		} else {
			return;
		}
	}

	/* pick whichever is closest */
	if (edid_abs(mode->clock - clock1) < edid_abs(mode->clock - clock2))
		clock = clock1;
	else
		clock = clock2;

	if (mode->clock == clock)
		return;

	mode->clock = clock;
}

static void do_detailed_mode(const struct detailed_timing *timing, void *c)
{
	struct detailed_mode_closure *closure = c;
	struct drm_display_mode newmode;

	if (!is_detailed_timing_descriptor(timing))
		return;

	if (!drm_mode_detailed(closure->conn, closure->drm_edid, timing, &newmode))
		return;

	if (closure->preferred)
		newmode.type |= DRM_MODE_TYPE_PREFERRED;

	fixup_detailed_cea_mode_clock(&newmode);

	closure->modes += edid_add_mode(closure->conn, &newmode);
	closure->preferred = false;
}

/*
 * add_detailed_modes - Add modes from detailed timings
 */
static int add_detailed_modes(struct drm_edid_conn *conn,
			      const struct drm_edid *drm_edid)
{
	struct detailed_mode_closure closure = {
		.conn = conn,
		.drm_edid = drm_edid,
	};

	if (drm_edid->edid->revision >= 4)
		closure.preferred = true; /* first detailed timing is always preferred */
	else
		closure.preferred =
			(drm_edid->edid->features & DRM_EDID_FEATURE_PREFERRED_TIMING) != 0;

	drm_for_each_detailed_block(drm_edid, do_detailed_mode, &closure);

	return closure.modes;
}

/* Return true if the EDID has a CTA extension or a DisplayID CTA data block */
static bool drm_edid_has_cta_extension(const struct drm_edid *drm_edid)
{
	const struct displayid_block *block;
	struct displayid_iter iter;
	struct drm_edid_iter edid_iter;
	const uint8_t *ext;
	bool found = false;

	/* Look for a top level CEA extension block */
	drm_edid_iter_begin(drm_edid, &edid_iter);
	drm_edid_iter_for_each(ext, &edid_iter) {
		if (ext[0] == CEA_EXT) {
			found = true;
			break;
		}
	}
	drm_edid_iter_end(&edid_iter);

	if (found)
		return true;

	/* CEA blocks can also be found embedded in a DisplayID block */
	displayid_iter_edid_begin(drm_edid, &iter);
	displayid_iter_for_each(block, &iter) {
		if (block->tag == DATA_BLOCK_CTA) {
			found = true;
			break;
		}
	}
	displayid_iter_end(&iter);

	return found;
}

/*
 * The 59.94 Hz twin of every 60 Hz CEA/HDMI mode found so far (and the
 * other way round): a sink that lists one takes both.
 */
static int add_alternate_cea_modes(struct drm_edid_conn *conn,
				   const struct drm_edid *drm_edid)
{
	int i, n, modes = 0;

	/* Don't add CTA modes if the CTA extension block is missing */
	if (!drm_edid_has_cta_extension(drm_edid))
		return 0;

	/*
	 * Go through all probed modes and create a new mode
	 * with the alternate clock for certain CEA modes.
	 */
	n = conn->nmodes;
	for (i = 0; i < n; i++) {
		const struct drm_display_mode *mode = &conn->modes[i];
		const struct drm_display_mode *cea_mode = NULL;
		struct drm_display_mode newmode;
		uint8_t vic = drm_match_cea_mode(mode);
		unsigned int clock1, clock2 = 0;

		if (drm_valid_cea_vic(vic)) {
			cea_mode = drm_cea_mode_for_vic(vic);
			clock2 = drm_cea_mode_alternate_clock(cea_mode);
		} else {
			vic = drm_match_hdmi_mode(mode);
			if (drm_valid_hdmi_vic(vic)) {
				cea_mode = drm_hdmi_mode_for_vic(vic);
				clock2 = drm_cea_mode_alternate_clock(cea_mode);
			}
		}

		if (!cea_mode)
			continue;

		clock1 = (unsigned int)cea_mode->clock;

		if (clock1 == clock2)
			continue;

		if ((unsigned int)mode->clock != clock1 && (unsigned int)mode->clock != clock2)
			continue;

		newmode = *cea_mode;

		/* Carry over the stereo flags */
		newmode.flags |= mode->flags & DRM_MODE_FLAG_3D_MASK;

		/*
		 * The current mode could be either variant. Make
		 * sure to pick the "other" clock for the new mode.
		 */
		if ((unsigned int)mode->clock != clock1)
			newmode.clock = (int)clock1;
		else
			newmode.clock = (int)clock2;

		modes += edid_add_mode(conn, &newmode);
	}

	return modes;
}

static uint8_t svd_to_vic(uint8_t svd)
{
	/* 0-6 bit vic, 7th bit native mode indicator */
	if ((svd >= 1 && svd <= 64) || (svd >= 129 && svd <= 192))
		return svd & 127;

	return svd;
}

int drm_display_mode_from_cea_vic(uint8_t video_code, struct drm_display_mode *out)
{
	const struct drm_display_mode *cea_mode;

	cea_mode = drm_cea_mode_for_vic(video_code);
	if (!cea_mode)
		return -EINVAL;

	*out = *cea_mode;
	return 0;
}

/*
 * A display mode for the 0-based vic_index'th VIC across all CTA VDBs in
 * the EDID; false on errors.
 */
static bool drm_display_mode_from_vic_index(struct drm_edid_conn *conn, int vic_index,
					    struct drm_display_mode *out)
{
	const struct drm_display_info *info = conn->info;

	if (!info->vics || vic_index >= info->vics_len || !info->vics[vic_index])
		return false;

	return drm_display_mode_from_cea_vic(info->vics[vic_index], out) == 0;
}

/*
 * do_y420vdb_modes - Parse YCBCR 420 only modes
 *
 * Parse the CEA-861-F YCBCR 420 Video Data Block (Y420VDB)
 * which contains modes which can be supported in YCBCR 420
 * output format only.
 */
static int do_y420vdb_modes(struct drm_edid_conn *conn,
			    const uint8_t *svds, uint8_t svds_len)
{
	int modes = 0, i;

	for (i = 0; i < svds_len; i++) {
		uint8_t vic = svd_to_vic(svds[i]);

		if (!drm_valid_cea_vic(vic))
			continue;

		modes += edid_add_mode(conn, drm_cea_mode_for_vic(vic));
	}

	return modes;
}

/* Add modes based on VICs parsed in parse_cta_vdb() */
static int add_cta_vdb_modes(struct drm_edid_conn *conn)
{
	const struct drm_display_info *info = conn->info;
	struct drm_display_mode mode;
	int i, modes = 0;

	if (!info->vics)
		return 0;

	for (i = 0; i < info->vics_len; i++) {
		if (drm_display_mode_from_vic_index(conn, i, &mode))
			modes += edid_add_mode(conn, &mode);
	}

	return modes;
}

struct stereo_mandatory_mode {
	int width, height, vrefresh;
	unsigned int flags;
};

static const struct stereo_mandatory_mode stereo_mandatory_modes[] = {
	{ 1920, 1080, 24, DRM_MODE_FLAG_3D_TOP_AND_BOTTOM },
	{ 1920, 1080, 24, DRM_MODE_FLAG_3D_FRAME_PACKING },
	{ 1920, 1080, 50,
	  DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF },
	{ 1920, 1080, 60,
	  DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF },
	{ 1280, 720, 50, DRM_MODE_FLAG_3D_TOP_AND_BOTTOM },
	{ 1280, 720, 50, DRM_MODE_FLAG_3D_FRAME_PACKING },
	{ 1280, 720, 60, DRM_MODE_FLAG_3D_TOP_AND_BOTTOM },
	{ 1280, 720, 60, DRM_MODE_FLAG_3D_FRAME_PACKING }
};

static bool stereo_match_mandatory(const struct drm_display_mode *mode,
				   const struct stereo_mandatory_mode *stereo_mode)
{
	unsigned int interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;

	return mode->hdisplay == stereo_mode->width &&
	       mode->vdisplay == stereo_mode->height &&
	       interlaced == (stereo_mode->flags & DRM_MODE_FLAG_INTERLACE) &&
	       drm_mode_vrefresh(mode) == stereo_mode->vrefresh;
}

static int add_hdmi_mandatory_stereo_modes(struct drm_edid_conn *conn)
{
	int modes = 0, i, j, n = conn->nmodes;

	/* The stereo variants go after every mode found so far. */
	for (j = 0; j < n; j++) {
		const struct drm_display_mode *mode = &conn->modes[j];

		for (i = 0; i < (int)ARRAY_SIZE(stereo_mandatory_modes); i++) {
			const struct stereo_mandatory_mode *mandatory;
			struct drm_display_mode new_mode;

			if (!stereo_match_mandatory(mode,
						    &stereo_mandatory_modes[i]))
				continue;

			mandatory = &stereo_mandatory_modes[i];
			new_mode = *mode;
			new_mode.flags |= mandatory->flags;
			modes += edid_add_mode(conn, &new_mode);
		}
	}

	return modes;
}

static int add_hdmi_mode(struct drm_edid_conn *conn, uint8_t vic)
{
	/* An unknown HDMI VIC is ignored. */
	if (!drm_valid_hdmi_vic(vic))
		return 0;

	return edid_add_mode(conn, drm_hdmi_mode_for_vic(vic));
}

static int add_3d_struct_modes(struct drm_edid_conn *conn, uint16_t structure,
			       int vic_index)
{
	struct drm_display_mode newmode;
	int modes = 0;

	if (structure & (1 << 0)) {
		if (drm_display_mode_from_vic_index(conn, vic_index, &newmode)) {
			newmode.flags |= DRM_MODE_FLAG_3D_FRAME_PACKING;
			modes += edid_add_mode(conn, &newmode);
		}
	}
	if (structure & (1 << 6)) {
		if (drm_display_mode_from_vic_index(conn, vic_index, &newmode)) {
			newmode.flags |= DRM_MODE_FLAG_3D_TOP_AND_BOTTOM;
			modes += edid_add_mode(conn, &newmode);
		}
	}
	if (structure & (1 << 8)) {
		if (drm_display_mode_from_vic_index(conn, vic_index, &newmode)) {
			newmode.flags |= DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF;
			modes += edid_add_mode(conn, &newmode);
		}
	}

	return modes;
}

static int hdmi_vsdb_latency_length(const uint8_t *db)
{
	if (hdmi_vsdb_i_latency_present(db))
		return 4;
	else if (hdmi_vsdb_latency_present(db))
		return 2;
	else
		return 0;
}

/*
 * do_hdmi_vsdb_modes - Parse the HDMI Vendor Specific data block
 * @db: start of the CEA vendor specific block
 * @len: length of the CEA block payload, ie. one can access up to db[len]
 *
 * Parses the HDMI VSDB looking for modes to add.  This function also adds
 * the stereo 3d modes when applicable.
 */
static int do_hdmi_vsdb_modes(struct drm_edid_conn *conn, const uint8_t *db, uint8_t len)
{
	int modes = 0, offset = 0, i, multi_present = 0, multi_len;
	uint8_t vic_len, hdmi_3d_len = 0;
	uint16_t mask;
	uint16_t structure_all;

	if (len < 8)
		goto out;

	/* no HDMI_Video_Present */
	if (!(db[8] & (1 << 5)))
		goto out;

	offset += hdmi_vsdb_latency_length(db);

	/* the declared length is not long enough for the 2 first bytes
	 * of additional video format capabilities */
	if (len < (8 + offset + 2))
		goto out;

	/* 3D_Present */
	offset++;
	if (db[8 + offset] & (1 << 7)) {
		modes += add_hdmi_mandatory_stereo_modes(conn);

		/* 3D_Multi_present */
		multi_present = (db[8 + offset] & 0x60) >> 5;
	}

	offset++;
	vic_len = db[8 + offset] >> 5;
	hdmi_3d_len = db[8 + offset] & 0x1f;

	for (i = 0; i < vic_len && len >= (9 + offset + i); i++) {
		uint8_t vic;

		vic = db[9 + offset + i];
		modes += add_hdmi_mode(conn, vic);
	}
	offset += 1 + vic_len;

	if (multi_present == 1)
		multi_len = 2;
	else if (multi_present == 2)
		multi_len = 4;
	else
		multi_len = 0;

	if (len < (8 + offset + hdmi_3d_len - 1))
		goto out;

	if (hdmi_3d_len < multi_len)
		goto out;

	if (multi_present == 1 || multi_present == 2) {
		/* 3D_Structure_ALL */
		structure_all = (uint16_t)((db[8 + offset] << 8) | db[9 + offset]);

		/* check if 3D_MASK is present */
		if (multi_present == 2)
			mask = (uint16_t)((db[10 + offset] << 8) | db[11 + offset]);
		else
			mask = 0xffff;

		for (i = 0; i < 16; i++) {
			if (mask & (1 << i))
				modes += add_3d_struct_modes(conn,
							     structure_all, i);
		}
	}

	offset += multi_len;

	for (i = 0; i < (hdmi_3d_len - multi_len); i++) {
		int vic_index;
		struct drm_display_mode newmode;
		unsigned int newflag = 0;
		bool detail_present;

		detail_present = ((db[8 + offset + i] & 0x0f) > 7);

		if (detail_present && (i + 1 == hdmi_3d_len - multi_len))
			break;

		/* 2D_VIC_order_X */
		vic_index = db[8 + offset + i] >> 4;

		/* 3D_Structure_X */
		switch (db[8 + offset + i] & 0x0f) {
		case 0:
			newflag = DRM_MODE_FLAG_3D_FRAME_PACKING;
			break;
		case 6:
			newflag = DRM_MODE_FLAG_3D_TOP_AND_BOTTOM;
			break;
		case 8:
			/* 3D_Detail_X */
			if ((db[9 + offset + i] >> 4) == 1)
				newflag = DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF;
			break;
		}

		if (newflag != 0) {
			if (drm_display_mode_from_vic_index(conn, vic_index, &newmode)) {
				newmode.flags |= newflag;
				modes += edid_add_mode(conn, &newmode);
			}
		}

		if (detail_present)
			i++;
	}

out:
	return modes;
}

static int add_cea_modes(struct drm_edid_conn *conn,
			 const struct drm_edid *drm_edid)
{
	const struct cea_db *db;
	struct cea_db_iter iter;
	int modes;

	/* CTA VDB block VICs parsed earlier */
	modes = add_cta_vdb_modes(conn);

	cea_db_iter_edid_begin(drm_edid, &iter);
	cea_db_iter_for_each(db, &iter) {
		if (cea_db_is_hdmi_vsdb(db)) {
			modes += do_hdmi_vsdb_modes(conn, (const uint8_t *)db,
						    (uint8_t)cea_db_payload_len(db));
		} else if (cea_db_is_y420vdb(db)) {
			const uint8_t *vdb420 = (const uint8_t *)cea_db_data(db) + 1;

			/* Add 4:2:0(only) modes present in EDID */
			modes += do_y420vdb_modes(conn, vdb420,
						  (uint8_t)(cea_db_payload_len(db) - 1));
		}
	}
	cea_db_iter_end(&iter);

	return modes;
}

/*** DisplayID modes ***/

static void drm_mode_displayid_detailed(const struct displayid_detailed_timings_1 *timings,
					bool type_7, struct drm_display_mode *mode)
{
	unsigned int pixel_clock = (timings->pixel_clock[0] |
				    (timings->pixel_clock[1] << 8) |
				    (timings->pixel_clock[2] << 16)) + 1;
	unsigned int hactive = timings->hactive + 1u;
	unsigned int hblank = timings->hblank + 1u;
	unsigned int hsync = (timings->hsync & 0x7fffu) + 1;
	unsigned int hsync_width = timings->hsw + 1u;
	unsigned int vactive = timings->vactive + 1u;
	unsigned int vblank = timings->vblank + 1u;
	unsigned int vsync = (timings->vsync & 0x7fffu) + 1;
	unsigned int vsync_width = timings->vsw + 1u;
	bool hsync_positive = (timings->hsync & (1 << 15)) != 0;
	bool vsync_positive = (timings->vsync & (1 << 15)) != 0;

	edid_memset(mode, 0, sizeof(*mode));

	/* resolution is kHz for type VII, and 10 kHz for type I */
	mode->clock = (int)(type_7 ? pixel_clock : pixel_clock * 10);
	mode->hdisplay = (uint16_t)hactive;
	mode->hsync_start = (uint16_t)(mode->hdisplay + hsync);
	mode->hsync_end = (uint16_t)(mode->hsync_start + hsync_width);
	mode->htotal = (uint16_t)(mode->hdisplay + hblank);

	mode->vdisplay = (uint16_t)vactive;
	mode->vsync_start = (uint16_t)(mode->vdisplay + vsync);
	mode->vsync_end = (uint16_t)(mode->vsync_start + vsync_width);
	mode->vtotal = (uint16_t)(mode->vdisplay + vblank);

	mode->flags = 0;
	mode->flags |= hsync_positive ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
	mode->flags |= vsync_positive ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
	mode->type = DRM_MODE_TYPE_DRIVER;

	if (timings->flags & 0x80)
		mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
}

static int add_displayid_detailed_1_modes(struct drm_edid_conn *conn,
					  const struct displayid_block *block)
{
	const struct displayid_detailed_timing_block *det =
		(const struct displayid_detailed_timing_block *)block;
	int i;
	int num_timings;
	struct drm_display_mode newmode;
	int num_modes = 0;
	bool type_7 = block->tag == DATA_BLOCK_2_TYPE_7_DETAILED_TIMING;

	/* blocks must be multiple of 20 bytes length */
	if (block->num_bytes % 20)
		return 0;

	num_timings = block->num_bytes / 20;
	for (i = 0; i < num_timings; i++) {
		const struct displayid_detailed_timings_1 *timings = &det->timings[i];

		drm_mode_displayid_detailed(timings, type_7, &newmode);
		num_modes += edid_add_mode(conn, &newmode);
	}
	return num_modes;
}

static bool drm_mode_displayid_formula(const struct displayid_formula_timings_9 *timings,
				       struct drm_display_mode *mode)
{
	uint16_t hactive = (uint16_t)(timings->hactive + 1);
	uint16_t vactive = (uint16_t)(timings->vactive + 1);
	uint8_t timing_formula = timings->flags & 0x7;

	/* TODO: support RB-v2 & RB-v3 */
	if (timing_formula > 1)
		return false;

	/* Video-optimized (fractional) refresh is not implemented: the
	 * non-video-optimized rate is used. */

	if (drm_cvt_mode(mode, hactive, vactive, timings->vrefresh + 1,
			 timing_formula == 1, false, false))
		return false;

	/* TODO: interpret S3D flags */

	mode->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(mode);

	return true;
}

static int add_displayid_formula_modes(struct drm_edid_conn *conn,
				       const struct displayid_block *block)
{
	const struct displayid_formula_timing_block *formula_block =
		(const struct displayid_formula_timing_block *)block;
	int num_timings;
	struct drm_display_mode newmode;
	int num_modes = 0;
	int timing_size = 6 + ((formula_block->base.rev & 0x70) >> 4);

	/* extended blocks are not supported yet */
	if (timing_size != 6)
		return 0;

	if (block->num_bytes % timing_size)
		return 0;

	num_timings = block->num_bytes / timing_size;
	for (int i = 0; i < num_timings; i++) {
		const struct displayid_formula_timings_9 *timings = &formula_block->timings[i];

		if (!drm_mode_displayid_formula(timings, &newmode))
			continue;

		num_modes += edid_add_mode(conn, &newmode);
	}
	return num_modes;
}

static int add_displayid_detailed_modes(struct drm_edid_conn *conn,
					const struct drm_edid *drm_edid)
{
	const struct displayid_block *block;
	struct displayid_iter iter;
	int num_modes = 0;

	displayid_iter_edid_begin(drm_edid, &iter);
	displayid_iter_for_each(block, &iter) {
		if (block->tag == DATA_BLOCK_TYPE_1_DETAILED_TIMING ||
		    block->tag == DATA_BLOCK_2_TYPE_7_DETAILED_TIMING)
			num_modes += add_displayid_detailed_1_modes(conn, block);
		else if (block->tag == DATA_BLOCK_2_TYPE_9_FORMULA_TIMING ||
			 block->tag == DATA_BLOCK_2_TYPE_10_FORMULA_TIMING)
			num_modes += add_displayid_formula_modes(conn, block);
	}
	displayid_iter_end(&iter);

	return num_modes;
}

int drm_edid_add_modes(struct drm_edid_conn *conn, const struct drm_edid *drm_edid)
{
	int num_modes = 0;
	int first = conn->nmodes;

	if (!drm_edid)
		return 0;

	/*
	 * EDID spec says modes should be preferred in this order:
	 * - preferred detailed mode
	 * - other detailed modes from base block
	 * - detailed modes from extension blocks
	 * - CVT 3-byte code modes
	 * - standard timing codes
	 * - established timing codes
	 * - modes inferred from GTF or CVT range information
	 *
	 * We get this pretty much right.
	 *
	 * XXX order for additional mode types in extension blocks?
	 */
	num_modes += add_detailed_modes(conn, drm_edid);
	num_modes += add_cvt_modes(conn, drm_edid);
	num_modes += add_standard_modes(conn, drm_edid);
	num_modes += add_established_modes(conn, drm_edid);
	num_modes += add_cea_modes(conn, drm_edid);
	num_modes += add_alternate_cea_modes(conn, drm_edid);
	num_modes += add_displayid_detailed_modes(conn, drm_edid);
	if (drm_edid->edid->features & DRM_EDID_FEATURE_CONTINUOUS_FREQ)
		num_modes += add_inferred_modes(conn, drm_edid);

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_PREFER_LARGE_60) ||
	    drm_edid_has_internal_quirk(conn, EDID_QUIRK_PREFER_LARGE_75))
		edid_fixup_preferred(conn, first);

	return num_modes;
}

/*** the sink's capabilities ***/

static void drm_calculate_luminance_range(struct drm_display_info *info)
{
	const struct hdr_static_metadata *hdr_metadata =
		&info->hdr_sink_metadata.hdmi_type1;
	struct drm_luminance_range_info *luminance_range = &info->luminance_range;
	static const uint8_t pre_computed_values[] = {
		50, 51, 52, 53, 55, 56, 57, 58, 59, 61, 62, 63, 65, 66, 68, 69,
		71, 72, 74, 75, 77, 79, 81, 82, 84, 86, 88, 90, 92, 94, 96, 98
	};
	uint32_t max_avg, min_cll, max, min, q, r;

	if (!(hdr_metadata->metadata_type & BIT(HDMI_STATIC_METADATA_TYPE1)))
		return;

	max_avg = hdr_metadata->max_fall;
	min_cll = hdr_metadata->min_cll;

	/*
	 * From the specification (CTA-861-G), for calculating the maximum
	 * luminance we need to use:
	 *	Luminance = 50*2**(CV/32)
	 * Where CV is a one-byte value.
	 * For calculating this expression we may need float point precision;
	 * to avoid this complexity level, we take advantage that CV is divided
	 * by a constant. From the Euclids division algorithm, we know that CV
	 * can be written as: CV = 32*q + r. Next, we replace CV in the
	 * Luminance expression and get 50*(2**q)*(2**(r/32)), hence we just
	 * need to pre-compute the value of r/32. The values are
	 *	round(50 * 2 ** (cv / 32.0)) for cv in 0..31.
	 */
	q = max_avg >> 5;
	r = max_avg % 32;
	max = (1u << q) * pre_computed_values[r];

	/* min luminance: maxLum * (CV/255)^2 / 100 */
	q = DIV_ROUND_CLOSEST(min_cll, 255);
	min = max * DIV_ROUND_CLOSEST((q * q), 100);

	luminance_range->min_luminance = min;
	luminance_range->max_luminance = max;
}

static uint8_t eotf_supported(const uint8_t *edid_ext)
{
	return edid_ext[2] &
	       (BIT(HDMI_EOTF_TRADITIONAL_GAMMA_SDR) |
		BIT(HDMI_EOTF_TRADITIONAL_GAMMA_HDR) |
		BIT(HDMI_EOTF_SMPTE_ST2084) |
		BIT(HDMI_EOTF_BT_2100_HLG));
}

static uint8_t hdr_metadata_type(const uint8_t *edid_ext)
{
	return edid_ext[3] & BIT(HDMI_STATIC_METADATA_TYPE1);
}

static void drm_parse_hdr_metadata_block(struct drm_display_info *info, const uint8_t *db)
{
	struct hdr_static_metadata *hdr_metadata = &info->hdr_sink_metadata.hdmi_type1;
	uint16_t len;

	len = (uint16_t)cea_db_payload_len(db);

	hdr_metadata->eotf = eotf_supported(db);
	hdr_metadata->metadata_type = hdr_metadata_type(db);

	if (len >= 4)
		hdr_metadata->max_cll = db[4];
	if (len >= 5)
		hdr_metadata->max_fall = db[5];
	if (len >= 6) {
		hdr_metadata->min_cll = db[6];

		/* Calculate only when all values are available */
		drm_calculate_luminance_range(info);
	}
}

/* CTA-861 Video Data Block (CTA VDB) */
static void parse_cta_vdb(struct drm_display_info *info, const struct cea_db *db)
{
	int i, vic_index, len = cea_db_payload_len(db);
	const uint8_t *svds = cea_db_data(db);
	uint8_t *vics;

	if (!len)
		return;

	/* Gracefully handle multiple VDBs, however unlikely that is */
	vics = krealloc(info->vics, (size_t)(info->vics_len + len));
	if (!vics)
		return;

	vic_index = info->vics_len;
	info->vics_len += len;
	info->vics = vics;

	for (i = 0; i < len; i++) {
		uint8_t vic = svd_to_vic(svds[i]);

		if (!drm_valid_cea_vic(vic))
			vic = 0;

		info->vics[vic_index++] = vic;
	}
}

static void edid_bitmap_set(unsigned long *map, unsigned int bit)
{
	map[bit / (8 * sizeof(unsigned long))] |= 1UL << (bit % (8 * sizeof(unsigned long)));
}

/*
 * CTA-861 YCbCr 4:2:0 Capability Map Data Block (CTA Y420CMDB)
 *
 * Y420CMDB contains a bitmap which gives the index of CTA modes from CTA VDB,
 * which can support YCBCR 420 sampling output also (apart from RGB/YCBCR444
 * etc). For example, if the bit 0 in bitmap is set, first mode in VDB can
 * support YCBCR420 output too.
 */
static void parse_cta_y420cmdb(struct drm_display_info *info,
			       const struct cea_db *db, uint64_t *y420cmdb_map)
{
	int i, map_len = cea_db_payload_len(db) - 1;
	const uint8_t *data = (const uint8_t *)cea_db_data(db) + 1;
	uint64_t map = 0;

	if (map_len == 0) {
		/* All CEA modes support ycbcr420 sampling also.*/
		map = ~0ULL;
		goto out;
	}

	/*
	 * This map indicates which of the existing CEA block modes
	 * from VDB can support YCBCR420 output too. So if bit=0 is
	 * set, first mode from VDB can support YCBCR420 output too.
	 * We will parse and keep this map, before parsing VDB itself
	 * to avoid going through the same block again and again.
	 *
	 * Spec is not clear about max possible size of this block.
	 * Clamping max bitmap block size at 8 bytes. Every byte can
	 * address 8 CEA modes, in this way this map can address
	 * 8*8 = first 64 SVDs.
	 */
	if (map_len > 8)
		map_len = 8;

	for (i = 0; i < map_len; i++)
		map |= (uint64_t)data[i] << (8 * i);

out:
	if (map)
		info->color_formats |= DRM_COLOR_FORMAT_YCBCR420;

	*y420cmdb_map = map;
}

/*
 * Update y420_cmdb_modes based on previously parsed CTA VDB and Y420CMDB.
 *
 * Translate the y420cmdb_map based on VIC indexes to y420_cmdb_modes indexed
 * using the VICs themselves.
 */
static void update_cta_y420cmdb(struct drm_display_info *info, uint64_t y420cmdb_map)
{
	struct drm_hdmi_info *hdmi = &info->hdmi;
	int i, len = min_t(int, info->vics_len, 64);

	for (i = 0; i < len; i++) {
		uint8_t vic = info->vics[i];

		if (vic && (y420cmdb_map & BIT_ULL(i)))
			edid_bitmap_set(hdmi->y420_cmdb_modes, vic);
	}
}

/* CTA-861-H YCbCr 4:2:0 Video Data Block (CTA Y420VDB) */
static void parse_cta_y420vdb(struct drm_display_info *info, const struct cea_db *db)
{
	struct drm_hdmi_info *hdmi = &info->hdmi;
	const uint8_t *svds = (const uint8_t *)cea_db_data(db) + 1;
	int i;

	for (i = 0; i < cea_db_payload_len(db) - 1; i++) {
		uint8_t vic = svd_to_vic(svds[i]);

		if (!drm_valid_cea_vic(vic))
			continue;

		edid_bitmap_set(hdmi->y420_vdb_modes, vic);
		info->color_formats |= DRM_COLOR_FORMAT_YCBCR420;
	}
}

static void drm_parse_vcdb(struct drm_display_info *info, const uint8_t *db)
{
	if (db[2] & EDID_CEA_VCDB_QS)
		info->rgb_quant_range_selectable = true;
}

static void drm_get_max_frl_rate(int max_frl_rate, uint8_t *max_lanes,
				 uint8_t *max_rate_per_lane)
{
	switch (max_frl_rate) {
	case 1:
		*max_lanes = 3;
		*max_rate_per_lane = 3;
		break;
	case 2:
		*max_lanes = 3;
		*max_rate_per_lane = 6;
		break;
	case 3:
		*max_lanes = 4;
		*max_rate_per_lane = 6;
		break;
	case 4:
		*max_lanes = 4;
		*max_rate_per_lane = 8;
		break;
	case 5:
		*max_lanes = 4;
		*max_rate_per_lane = 10;
		break;
	case 6:
		*max_lanes = 4;
		*max_rate_per_lane = 12;
		break;
	case 0:
	default:
		*max_lanes = 0;
		*max_rate_per_lane = 0;
	}
}

static void drm_parse_ycbcr420_deep_color_info(struct drm_display_info *info,
					       const uint8_t *db)
{
	uint8_t dc_mask;
	struct drm_hdmi_info *hdmi = &info->hdmi;

	dc_mask = db[7] & DRM_EDID_YCBCR420_DC_MASK;
	hdmi->y420_dc_modes = dc_mask;
}

static void drm_parse_dsc_info(struct drm_hdmi_dsc_cap *hdmi_dsc,
			       const uint8_t *hf_scds)
{
	hdmi_dsc->v_1p2 = (hf_scds[11] & DRM_EDID_DSC_1P2) != 0;

	if (!hdmi_dsc->v_1p2)
		return;

	hdmi_dsc->native_420 = (hf_scds[11] & DRM_EDID_DSC_NATIVE_420) != 0;
	hdmi_dsc->all_bpp = (hf_scds[11] & DRM_EDID_DSC_ALL_BPP) != 0;

	if (hf_scds[11] & DRM_EDID_DSC_16BPC)
		hdmi_dsc->bpc_supported = 16;
	else if (hf_scds[11] & DRM_EDID_DSC_12BPC)
		hdmi_dsc->bpc_supported = 12;
	else if (hf_scds[11] & DRM_EDID_DSC_10BPC)
		hdmi_dsc->bpc_supported = 10;
	else
		/* Supports min 8 BPC if DSC 1.2 is supported*/
		hdmi_dsc->bpc_supported = 8;

	if (cea_db_payload_len(hf_scds) >= 12 && hf_scds[12]) {
		uint8_t dsc_max_slices;
		uint8_t dsc_max_frl_rate;

		dsc_max_frl_rate = (hf_scds[12] & DRM_EDID_DSC_MAX_FRL_RATE_MASK) >> 4;
		drm_get_max_frl_rate(dsc_max_frl_rate, &hdmi_dsc->max_lanes,
				     &hdmi_dsc->max_frl_rate_per_lane);

		dsc_max_slices = hf_scds[12] & DRM_EDID_DSC_MAX_SLICES;

		switch (dsc_max_slices) {
		case 1:
			hdmi_dsc->max_slices = 1;
			hdmi_dsc->clk_per_slice = 340;
			break;
		case 2:
			hdmi_dsc->max_slices = 2;
			hdmi_dsc->clk_per_slice = 340;
			break;
		case 3:
			hdmi_dsc->max_slices = 4;
			hdmi_dsc->clk_per_slice = 340;
			break;
		case 4:
			hdmi_dsc->max_slices = 8;
			hdmi_dsc->clk_per_slice = 340;
			break;
		case 5:
			hdmi_dsc->max_slices = 8;
			hdmi_dsc->clk_per_slice = 400;
			break;
		case 6:
			hdmi_dsc->max_slices = 12;
			hdmi_dsc->clk_per_slice = 400;
			break;
		case 7:
			hdmi_dsc->max_slices = 16;
			hdmi_dsc->clk_per_slice = 400;
			break;
		case 0:
		default:
			hdmi_dsc->max_slices = 0;
			hdmi_dsc->clk_per_slice = 0;
		}
	}

	if (cea_db_payload_len(hf_scds) >= 13 && hf_scds[13])
		hdmi_dsc->total_chunk_kbytes = hf_scds[13] & DRM_EDID_DSC_TOTAL_CHUNK_KBYTES;
}

/* Sink Capability Data Structure */
static void drm_parse_hdmi_forum_scds(struct drm_display_info *info,
				      const uint8_t *hf_scds)
{
	struct drm_hdmi_info *hdmi = &info->hdmi;
	struct drm_hdmi_dsc_cap *hdmi_dsc = &hdmi->dsc_cap;
	int max_tmds_clock = 0;
	uint8_t max_frl_rate = 0;

	info->has_hdmi_infoframe = true;

	if (hf_scds[6] & 0x80) {
		hdmi->scdc.supported = true;
		if (hf_scds[6] & 0x40)
			hdmi->scdc.read_request = true;
	}

	/*
	 * All HDMI 2.0 monitors must support scrambling at rates > 340 MHz.
	 * And as per the spec, three factors confirm this:
	 * * Availability of a HF-VSDB block in EDID (check)
	 * * Non zero Max_TMDS_Char_Rate filed in HF-VSDB (let's check)
	 * * SCDC support available (let's check)
	 * Lets check it out.
	 */

	if (hf_scds[5]) {
		struct drm_scdc *scdc = &hdmi->scdc;

		/* max clock is 5000 KHz times block value */
		max_tmds_clock = hf_scds[5] * 5000;

		if (max_tmds_clock > 340000)
			info->max_tmds_clock = max_tmds_clock;

		if (scdc->supported) {
			scdc->scrambling.supported = true;

			/* Few sinks support scrambling for clocks < 340M */
			if ((hf_scds[6] & 0x8))
				scdc->scrambling.low_rates = true;
		}
	}

	if (hf_scds[7]) {
		max_frl_rate = (hf_scds[7] & DRM_EDID_MAX_FRL_RATE_MASK) >> 4;
		drm_get_max_frl_rate(max_frl_rate, &hdmi->max_lanes,
				     &hdmi->max_frl_rate_per_lane);
	}

	drm_parse_ycbcr420_deep_color_info(info, hf_scds);

	if (cea_db_payload_len(hf_scds) >= 11 && hf_scds[11])
		drm_parse_dsc_info(hdmi_dsc, hf_scds);
}

static void drm_parse_hdmi_deep_color_info(struct drm_display_info *info,
					   const uint8_t *hdmi)
{
	unsigned int dc_bpc = 0;

	/* HDMI supports at least 8 bpc */
	info->bpc = 8;

	if (cea_db_payload_len(hdmi) < 6)
		return;

	if (hdmi[6] & DRM_EDID_HDMI_DC_30) {
		dc_bpc = 10;
		info->edid_hdmi_rgb444_dc_modes |= DRM_EDID_HDMI_DC_30;
	}

	if (hdmi[6] & DRM_EDID_HDMI_DC_36) {
		dc_bpc = 12;
		info->edid_hdmi_rgb444_dc_modes |= DRM_EDID_HDMI_DC_36;
	}

	if (hdmi[6] & DRM_EDID_HDMI_DC_48) {
		dc_bpc = 16;
		info->edid_hdmi_rgb444_dc_modes |= DRM_EDID_HDMI_DC_48;
	}

	/* No deep color support on this HDMI sink. */
	if (dc_bpc == 0)
		return;

	info->bpc = dc_bpc;

	/* YCRCB444 is optional according to spec. */
	if (hdmi[6] & DRM_EDID_HDMI_DC_Y444)
		info->edid_hdmi_ycbcr444_dc_modes = info->edid_hdmi_rgb444_dc_modes;

	/*
	 * Spec says that if any deep color mode is supported at all,
	 * then deep color 36 bit must be supported -- a sink that lists
	 * deep colour without DC_36 is taken at its word all the same.
	 */
}

/* HDMI Vendor-Specific Data Block (HDMI VSDB, H14b-VSDB) */
static void drm_parse_hdmi_vsdb_video(struct drm_display_info *info, const uint8_t *db)
{
	uint8_t len = (uint8_t)cea_db_payload_len(db);

	info->is_hdmi = true;

	info->source_physical_address = (uint16_t)((db[4] << 8) | db[5]);

	if (len >= 6)
		info->dvi_dual = db[6] & 1;
	if (len >= 7)
		info->max_tmds_clock = db[7] * 5000;

	/*
	 * Try to infer whether the sink supports HDMI infoframes.
	 *
	 * HDMI infoframe support was first added in HDMI 1.4. Assume the sink
	 * supports infoframes if HDMI_Video_present is set.
	 */
	if (len >= 8 && db[8] & BIT(5))
		info->has_hdmi_infoframe = true;

	drm_parse_hdmi_deep_color_info(info, db);
}

/*
 * The EDID extension for head-mounted and specialized monitors: version
 * 1 and 2 are HMDs, version 3 flags desktop usage explicitly.
 */
static void drm_parse_microsoft_vsdb(struct drm_display_info *info, const uint8_t *db)
{
	uint8_t version = db[4];
	bool desktop_usage = (db[5] & BIT(6)) != 0;

	if (version == 1 || version == 2 || (version == 3 && !desktop_usage))
		info->non_desktop = true;
}

static void drm_parse_cea_ext(struct drm_display_info *info,
			      const struct drm_edid *drm_edid)
{
	struct drm_edid_iter edid_iter;
	const struct cea_db *db;
	struct cea_db_iter iter;
	const uint8_t *edid_ext;
	uint64_t y420cmdb_map = 0;

	drm_edid_iter_begin(drm_edid, &edid_iter);
	drm_edid_iter_for_each(edid_ext, &edid_iter) {
		if (edid_ext[0] != CEA_EXT)
			continue;

		/* the first CTA extension's revision counts; a mismatch in a
		 * later one is tolerated */
		if (!info->cea_rev)
			info->cea_rev = edid_ext[1];

		/* The existence of a CTA extension should imply RGB support */
		info->color_formats = DRM_COLOR_FORMAT_RGB444;
		if (edid_ext[3] & EDID_CEA_YCRCB444)
			info->color_formats |= DRM_COLOR_FORMAT_YCBCR444;
		if (edid_ext[3] & EDID_CEA_YCRCB422)
			info->color_formats |= DRM_COLOR_FORMAT_YCBCR422;
		if (edid_ext[3] & EDID_BASIC_AUDIO)
			info->has_audio = true;
	}
	drm_edid_iter_end(&edid_iter);

	cea_db_iter_edid_begin(drm_edid, &iter);
	cea_db_iter_for_each(db, &iter) {
		const uint8_t *data = (const uint8_t *)db;

		if (cea_db_is_hdmi_vsdb(db))
			drm_parse_hdmi_vsdb_video(info, data);
		else if (cea_db_is_hdmi_forum_vsdb(db) ||
			 cea_db_is_hdmi_forum_scdb(db))
			drm_parse_hdmi_forum_scds(info, data);
		else if (cea_db_is_microsoft_vsdb(db))
			drm_parse_microsoft_vsdb(info, data);
		else if (cea_db_is_y420cmdb(db))
			parse_cta_y420cmdb(info, db, &y420cmdb_map);
		else if (cea_db_is_y420vdb(db))
			parse_cta_y420vdb(info, db);
		else if (cea_db_is_vcdb(db))
			drm_parse_vcdb(info, data);
		else if (cea_db_is_hdmi_hdr_metadata_block(db))
			drm_parse_hdr_metadata_block(info, data);
		else if (cea_db_tag(db) == CTA_DB_VIDEO)
			parse_cta_vdb(info, db);
		else if (cea_db_tag(db) == CTA_DB_AUDIO)
			info->has_audio = true;
	}
	cea_db_iter_end(&iter);

	if (y420cmdb_map)
		update_cta_y420cmdb(info, y420cmdb_map);
}

struct monitor_range_closure {
	struct drm_display_info *info;
	const struct drm_edid *drm_edid;
};

static void get_monitor_range(const struct detailed_timing *timing, void *c)
{
	struct monitor_range_closure *closure = c;
	struct drm_monitor_range_info *monitor_range = &closure->info->monitor_range;
	const struct detailed_non_pixel *data = &timing->data.other_data;
	const struct detailed_data_monitor_range *range = &data->data.range;
	const struct edid *edid = closure->drm_edid->edid;

	if (!is_display_descriptor(timing, EDID_DETAIL_MONITOR_RANGE))
		return;

	/*
	 * These limits are used to determine the VRR refresh
	 * rate range. Only the "range limits only" variant
	 * of the range descriptor seems to guarantee that
	 * any and all timings are accepted by the sink, as
	 * opposed to just timings conforming to the indicated
	 * formula (GTF/GTF2/CVT). Thus other variants of the
	 * range descriptor are not accepted here.
	 */
	if (range->flags != DRM_EDID_RANGE_LIMITS_ONLY_FLAG)
		return;

	monitor_range->min_vfreq = range->min_vfreq;
	monitor_range->max_vfreq = range->max_vfreq;

	if (edid->revision >= 4) {
		if (data->pad2 & DRM_EDID_RANGE_OFFSET_MIN_VFREQ)
			monitor_range->min_vfreq += 255;
		if (data->pad2 & DRM_EDID_RANGE_OFFSET_MAX_VFREQ)
			monitor_range->max_vfreq += 255;
	}
}

static void drm_get_monitor_range(struct drm_display_info *info,
				  const struct drm_edid *drm_edid)
{
	struct monitor_range_closure closure = {
		.info = info,
		.drm_edid = drm_edid,
	};

	if (drm_edid->edid->revision < 4)
		return;

	if (!(drm_edid->edid->features & DRM_EDID_FEATURE_CONTINUOUS_FREQ))
		return;

	drm_for_each_detailed_block(drm_edid, get_monitor_range, &closure);
}

static void drm_parse_vesa_mso_data(struct drm_display_info *info,
				    const struct displayid_block *block)
{
	const struct displayid_vesa_vendor_specific_block *vesa =
		(const struct displayid_vesa_vendor_specific_block *)block;

	if (block->num_bytes < 3)
		return;

	if (oui(vesa->oui[0], vesa->oui[1], vesa->oui[2]) != VESA_IEEE_OUI)
		return;

	if (sizeof(*vesa) != sizeof(*block) + block->num_bytes)
		return;

	switch (FIELD_GET(DISPLAYID_VESA_MSO_MODE, vesa->mso)) {
	default:
		/* reserved MSO mode value */
		fallthrough;
	case 0:
		info->mso_stream_count = 0;
		break;
	case 1:
		info->mso_stream_count = 2; /* 2 or 4 links */
		break;
	case 2:
		info->mso_stream_count = 4; /* 4 links */
		break;
	}

	if (!info->mso_stream_count) {
		info->mso_pixel_overlap = 0;
		return;
	}

	info->mso_pixel_overlap = (uint8_t)FIELD_GET(DISPLAYID_VESA_MSO_OVERLAP, vesa->mso);
	if (info->mso_pixel_overlap > 8) /* reserved value */
		info->mso_pixel_overlap = 8;
}

static void drm_update_mso(struct drm_display_info *info,
			   const struct drm_edid *drm_edid)
{
	const struct displayid_block *block;
	struct displayid_iter iter;

	displayid_iter_edid_begin(drm_edid, &iter);
	displayid_iter_for_each(block, &iter) {
		if (block->tag == DATA_BLOCK_2_VENDOR_SPECIFIC)
			drm_parse_vesa_mso_data(info, block);
	}
	displayid_iter_end(&iter);
}

/*
 * A connector without EDID information: reset all of the values which
 * would have been set from an EDID.  The driver's own fields (subpixel
 * order, panel orientation) are left alone.
 */
void drm_display_info_reset(struct drm_display_info *info)
{
	info->width_mm = 0;
	info->height_mm = 0;

	info->bpc = 0;
	info->color_formats = 0;
	info->cea_rev = 0;
	info->max_tmds_clock = 0;
	info->dvi_dual = false;
	info->is_hdmi = false;
	info->has_audio = false;
	info->has_hdmi_infoframe = false;
	info->rgb_quant_range_selectable = false;
	edid_memset(&info->hdmi, 0, sizeof(info->hdmi));
	edid_memset(&info->hdr_sink_metadata, 0, sizeof(info->hdr_sink_metadata));

	info->edid_hdmi_rgb444_dc_modes = 0;
	info->edid_hdmi_ycbcr444_dc_modes = 0;

	info->non_desktop = 0;
	edid_memset(&info->monitor_range, 0, sizeof(info->monitor_range));
	edid_memset(&info->luminance_range, 0, sizeof(info->luminance_range));

	info->mso_stream_count = 0;
	info->mso_pixel_overlap = 0;
	info->max_dsc_bpp = 0;

	if (info->vics)
		kfree(info->vics);
	info->vics = NULL;
	info->vics_len = 0;

	info->quirks = 0;

	info->source_physical_address = CEC_PHYS_ADDR_INVALID;
}

static void drm_displayid_process_base_section_header(struct drm_display_info *info,
						      const struct displayid_iter *iter)
{
	if (displayid_version(iter) == DISPLAY_ID_STRUCTURE_VER_20 &&
	    (displayid_primary_use(iter) == PRIMARY_USE_HEAD_MOUNTED_VR ||
	     displayid_primary_use(iter) == PRIMARY_USE_HEAD_MOUNTED_AR))
		info->non_desktop = true;
}

static void update_displayid_info(struct drm_display_info *info,
				  const struct drm_edid *drm_edid)
{
	const struct displayid_block *block;
	struct displayid_iter iter;
	bool base_section_header_processed = false;

	displayid_iter_edid_begin(drm_edid, &iter);
	displayid_iter_for_each(block, &iter) {
		(void)block;
		if (!base_section_header_processed) {
			drm_displayid_process_base_section_header(info, &iter);
			base_section_header_processed = true;
		}
	}
	displayid_iter_end(&iter);
}

static void update_display_info(struct drm_edid_conn *conn,
				const struct drm_edid *drm_edid)
{
	struct drm_display_info *info = conn->info;
	const struct edid *edid;

	drm_display_info_reset(info);
	clear_eld(conn);

	if (!drm_edid)
		return;

	edid = drm_edid->edid;

	info->quirks = edid_get_quirks(drm_edid);

	info->width_mm = edid->width_cm * 10u;
	info->height_mm = edid->height_cm * 10u;

	drm_get_monitor_range(info, drm_edid);

	if (edid->revision < 3)
		goto out;

	if (!drm_edid_is_digital(drm_edid))
		goto out;

	info->color_formats |= DRM_COLOR_FORMAT_RGB444;
	drm_parse_cea_ext(info, drm_edid);

	update_displayid_info(info, drm_edid);

	/*
	 * Digital sink with "DFP 1.x compliant TMDS" according to EDID 1.3?
	 *
	 * For such displays, the DFP spec 1.0, section 3.10 "EDID support"
	 * tells us to assume 8 bpc color depth if the EDID doesn't have
	 * extensions which tell otherwise.
	 */
	if (info->bpc == 0 && edid->revision == 3 &&
	    edid->input & DRM_EDID_DIGITAL_DFP_1_X)
		info->bpc = 8;

	/* Only defined for 1.4 with digital displays */
	if (edid->revision < 4)
		goto out;

	switch (edid->input & DRM_EDID_DIGITAL_DEPTH_MASK) {
	case DRM_EDID_DIGITAL_DEPTH_6:
		info->bpc = 6;
		break;
	case DRM_EDID_DIGITAL_DEPTH_8:
		info->bpc = 8;
		break;
	case DRM_EDID_DIGITAL_DEPTH_10:
		info->bpc = 10;
		break;
	case DRM_EDID_DIGITAL_DEPTH_12:
		info->bpc = 12;
		break;
	case DRM_EDID_DIGITAL_DEPTH_14:
		info->bpc = 14;
		break;
	case DRM_EDID_DIGITAL_DEPTH_16:
		info->bpc = 16;
		break;
	case DRM_EDID_DIGITAL_DEPTH_UNDEF:
	default:
		info->bpc = 0;
		break;
	}

	if (edid->features & DRM_EDID_FEATURE_RGB_YCRCB444)
		info->color_formats |= DRM_COLOR_FORMAT_YCBCR444;
	if (edid->features & DRM_EDID_FEATURE_RGB_YCRCB422)
		info->color_formats |= DRM_COLOR_FORMAT_YCBCR422;

	drm_update_mso(info, drm_edid);

out:
	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_NON_DESKTOP))
		info->non_desktop = true;

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_CAP_DSC_15BPP))
		info->max_dsc_bpp = 15;

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_FORCE_6BPC))
		info->bpc = 6;

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_FORCE_8BPC))
		info->bpc = 8;

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_FORCE_10BPC))
		info->bpc = 10;

	if (drm_edid_has_internal_quirk(conn, EDID_QUIRK_FORCE_12BPC))
		info->bpc = 12;

	/* Depends on info->cea_rev set by drm_parse_cea_ext() above */
	drm_edid_to_eld(conn, drm_edid);
}

/*** tiled displays ***/

static void drm_parse_tiled_block(struct drm_edid_tile *t,
				  const struct displayid_block *block)
{
	const struct displayid_tiled_block *tile = (const struct displayid_tiled_block *)block;
	uint16_t w, h;
	uint8_t tile_v_loc, tile_h_loc;
	uint8_t num_v_tile, num_h_tile;

	/* tiled block payload per spec: cap 1 + topo 3 + size 4 + bezel 5 + id 9 = 22 */
	if (block->num_bytes < 22)
		return;

	w = (uint16_t)(tile->tile_size[0] | tile->tile_size[1] << 8);
	h = (uint16_t)(tile->tile_size[2] | tile->tile_size[3] << 8);

	num_v_tile = (tile->topo[0] & 0xf) | (tile->topo[2] & 0x30);
	num_h_tile = (tile->topo[0] >> 4) | ((tile->topo[2] >> 2) & 0x30);
	tile_v_loc = (tile->topo[1] & 0xf) | ((tile->topo[2] & 0x3) << 4);
	tile_h_loc = (tile->topo[1] >> 4) | (((tile->topo[2] >> 2) & 0x3) << 4);

	t->has_tile = true;
	if (tile->tile_cap & 0x80)
		t->tile_is_single_monitor = true;

	t->num_h_tile = (uint8_t)(num_h_tile + 1);
	t->num_v_tile = (uint8_t)(num_v_tile + 1);
	t->tile_h_loc = tile_h_loc;
	t->tile_v_loc = tile_v_loc;
	t->tile_h_size = (uint16_t)(w + 1);
	t->tile_v_size = (uint16_t)(h + 1);
	/* the group's id: vendor, product and serial of the topology */
	edid_memcpy(t->topology_id, tile->topology_id, sizeof(t->topology_id));
}

static bool displayid_is_tiled_block(const struct displayid_iter *iter,
				     const struct displayid_block *block)
{
	return (displayid_version(iter) < DISPLAY_ID_STRUCTURE_VER_20 &&
		block->tag == DATA_BLOCK_TILED_DISPLAY) ||
	       (displayid_version(iter) == DISPLAY_ID_STRUCTURE_VER_20 &&
		block->tag == DATA_BLOCK_2_TILED_DISPLAY_TOPOLOGY);
}

static void update_tile_info(struct drm_edid_conn *conn,
			     const struct drm_edid *drm_edid)
{
	const struct displayid_block *block;
	struct displayid_iter iter;

	edid_memset(&conn->tile, 0, sizeof(conn->tile));

	displayid_iter_edid_begin(drm_edid, &iter);
	displayid_iter_for_each(block, &iter) {
		if (displayid_is_tiled_block(&iter, block))
			drm_parse_tiled_block(&conn->tile, block);
	}
	displayid_iter_end(&iter);
}

void drm_edid_update_display_info(struct drm_edid_conn *conn,
				  const struct drm_edid *drm_edid)
{
	update_display_info(conn, drm_edid);
	update_tile_info(conn, drm_edid);
}

/*** small queries ***/

bool drm_edid_has_quirk(const struct drm_display_info *info, enum drm_edid_quirk quirk)
{
	return (info->quirks & BIT(quirk)) != 0;
}

int drm_add_modes_noedid(struct drm_edid_conn *conn, unsigned int hdisplay,
			 unsigned int vdisplay)
{
	int i, count = drm_dmt_num_modes(), num_modes = 0;

	for (i = 0; i < count; i++) {
		const struct drm_display_mode *ptr = drm_dmt_mode(i);

		if (hdisplay && vdisplay) {
			/*
			 * Only when two are valid, they will be used to check
			 * whether the mode should be added to the mode list of
			 * the connector.
			 */
			if (ptr->hdisplay > hdisplay ||
			    ptr->vdisplay > vdisplay)
				continue;
		}
		if (drm_mode_vrefresh(ptr) > 61)
			continue;
		if (drm_edid_probed_add(conn, ptr))
			num_modes++;
	}
	return num_modes;
}

/* All CEA modes other than VIC 1 use limited quantization range. */
enum hdmi_quantization_range drm_default_rgb_quant_range(const struct drm_display_mode *mode)
{
	return drm_match_cea_mode(mode) > 1 ?
		HDMI_QUANTIZATION_RANGE_LIMITED :
		HDMI_QUANTIZATION_RANGE_FULL;
}

static bool _drm_detect_hdmi_monitor(const struct drm_edid *drm_edid)
{
	const struct cea_db *db;
	struct cea_db_iter iter;
	bool hdmi = false;

	/*
	 * Because HDMI identifier is in Vendor Specific Block,
	 * search it from all data blocks of CEA extension.
	 */
	cea_db_iter_edid_begin(drm_edid, &iter);
	cea_db_iter_for_each(db, &iter) {
		if (cea_db_is_hdmi_vsdb(db)) {
			hdmi = true;
			break;
		}
	}
	cea_db_iter_end(&iter);

	return hdmi;
}

bool drm_detect_hdmi_monitor(const struct edid *edid)
{
	struct drm_edid drm_edid;

	return _drm_detect_hdmi_monitor(drm_edid_legacy_init(&drm_edid, edid));
}

/*
 * A sink with "basic audio" but no CTA audio block takes basic audio only;
 * one with an audio block and a format takes at least basic audio, even
 * when the bit is not set.
 */
static bool _drm_detect_monitor_audio(const struct drm_edid *drm_edid)
{
	struct drm_edid_iter edid_iter;
	const struct cea_db *db;
	struct cea_db_iter iter;
	const uint8_t *edid_ext;
	bool has_audio = false;

	drm_edid_iter_begin(drm_edid, &edid_iter);
	drm_edid_iter_for_each(edid_ext, &edid_iter) {
		if (edid_ext[0] == CEA_EXT) {
			has_audio = (edid_ext[3] & EDID_BASIC_AUDIO) != 0;
			if (has_audio)
				break;
		}
	}
	drm_edid_iter_end(&edid_iter);

	if (has_audio)
		goto end;

	cea_db_iter_edid_begin(drm_edid, &iter);
	cea_db_iter_for_each(db, &iter) {
		if (cea_db_tag(db) == CTA_DB_AUDIO) {
			has_audio = true;
			break;
		}
	}
	cea_db_iter_end(&iter);

end:
	return has_audio;
}

bool drm_detect_monitor_audio(const struct edid *edid)
{
	struct drm_edid drm_edid;

	return _drm_detect_monitor_audio(drm_edid_legacy_init(&drm_edid, edid));
}

int drm_edid_parse_full(const uint8_t *edid, unsigned len, struct drm_edid_conn *conn)
{
	const struct drm_edid *drm_edid;
	int n;

	if (!edid || len < EDID_LENGTH)
		return -EINVAL;
	drm_edid = drm_edid_alloc_checked(edid, len);
	if (!drm_edid)
		return -EINVAL;

	drm_edid_update_display_info(conn, drm_edid);
	n = drm_edid_add_modes(conn, drm_edid);

	drm_edid_free(drm_edid);
	return n;
}

/*** the drivers' fixed-size summary ***/

/* Room for the full parse before it is reduced to DRM_EDID_MAX_MODES. */
#define EDID_PARSE_WORK_MODES 128

struct edid_parse_work {
	struct drm_display_info info;
	uint8_t eld[DRM_ELD_BUF_SIZE];
	struct drm_edid_conn conn;
	struct drm_display_mode modes[EDID_PARSE_WORK_MODES];
};

static void copy_text(char *dst, unsigned cap, const char *src, int n)
{
	unsigned k = 0;

	for (int i = 0; i < n && k + 1 < cap; i++) {
		uint8_t c = (uint8_t)src[i];
		if (c == 0x0a || c == 0)
			break;
		if (c < 0x20 || c > 0x7e)
			c = ' ';
		dst[k++] = (char)c;
	}
	while (k > 0 && dst[k - 1] == ' ')
		k--;
	dst[k] = 0;
}

/* The base block's range limits descriptor, as the connector filters the
 * standard modes with (the last one wins). */
static void parse_range(const uint8_t *d, uint8_t version, uint8_t revision,
			struct drm_edid_info *out)
{
	out->has_range = 1;
	out->range.min_vrefresh = d[5];
	out->range.max_vrefresh = d[6];
	out->range.min_hfreq = d[7];
	out->range.max_hfreq = d[8];
	out->range.max_clock_khz = (uint32_t)d[9] * 10000;
	/* EDID 1.4 offset flags in byte 4 */
	if (version == 1 && revision >= 4) {
		if (d[4] & 0x02)
			out->range.max_vrefresh += 255;
		if ((d[4] & 0x03) == 0x03)
			out->range.min_vrefresh += 255;
		if (d[4] & 0x08)
			out->range.max_hfreq += 255;
		if ((d[4] & 0x0c) == 0x0c)
			out->range.min_hfreq += 255;
	}
}

/* Can a client that knows nothing of stereo, picture aspect or 4:2:0, on
 * a source that sends RGB without pixel repetition, use the mode? */
static bool edid_mode_is_plain(const struct drm_display_info *info,
			       const struct drm_display_mode *mode)
{
	if (mode->flags & DRM_MODE_FLAG_3D_MASK)
		return false;
	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		return false;
	if (drm_mode_is_420_only(info, mode))
		return false;
	return true;
}

int drm_edid_parse(const uint8_t *edid, unsigned len, struct drm_edid_info *out)
{
	const struct drm_edid *drm_edid;
	const struct edid *base;
	struct edid_parse_work *w;
	int first_detailed = -1, i, j;
	bool dtd0_is_mode0;
	struct drm_display_mode dtd0;
	char name[13];
	int mnl;

	edid_memset(out, 0, sizeof(*out));
	out->preferred = -1;
	if (!edid || len < DRM_EDID_BLOCK)
		return -EINVAL;

	drm_edid = drm_edid_alloc_checked(edid, len);
	if (!drm_edid)
		return -EINVAL;
	w = kalloc(sizeof(*w));
	if (!w) {
		drm_edid_free(drm_edid);
		return -ENOMEM;
	}
	edid_memset(w, 0, sizeof(*w));
	w->conn.info = &w->info;
	w->conn.eld = w->eld;
	w->conn.modes = w->modes;
	w->conn.max_modes = EDID_PARSE_WORK_MODES;
	base = drm_edid->edid;

	/* identity */
	out->version = base->version;
	out->revision = base->revision;
	drm_edid_decode_mfg_id((uint16_t)((base->mfg_id[0] << 8) | base->mfg_id[1]),
			       out->vendor);
	out->product = (uint16_t)EDID_PRODUCT_ID(base);
	out->serial = base->serial;
	mnl = get_monitor_name(drm_edid, name);
	copy_text(out->name, sizeof(out->name), name, mnl);
	if (base->width_cm && base->height_cm) {
		out->mm_width = base->width_cm * 10u;
		out->mm_height = base->height_cm * 10u;
	}
	out->digital = !!(base->input & DRM_EDID_INPUT_DIGITAL);
	for (i = 0; i < EDID_DETAILED_TIMINGS; i++) {
		const struct detailed_timing *d = &base->detailed_timings[i];

		if (is_display_descriptor(d, EDID_DETAIL_MONITOR_RANGE))
			parse_range((const uint8_t *)d, base->version, base->revision, out);
	}

	/* the sink */
	drm_edid_update_display_info(&w->conn, drm_edid);
	out->bpc = (int)w->info.bpc;
	out->is_hdmi = w->info.is_hdmi;
	out->has_audio = w->info.has_audio;
	{
		struct drm_edid_iter it;
		const uint8_t *ext;

		drm_edid_iter_begin(drm_edid, &it);
		drm_edid_iter_for_each(ext, &it) {
			if (ext[0] == CEA_EXT && ext[1] >= 2)
				out->cea_underscan = !!(ext[3] & 0x80);
		}
		drm_edid_iter_end(&it);
	}

	/* the modes: detailed timings come first in the full list */
	drm_edid_add_modes(&w->conn, drm_edid);
	/* The first mode of the full list is the base block's first
	 * descriptor when that is a detailed timing that makes a mode. */
	dtd0_is_mode0 = is_detailed_timing_descriptor(&base->detailed_timings[0]) &&
			drm_mode_detailed(&w->conn, drm_edid, &base->detailed_timings[0], &dtd0);
	for (i = 0; i < w->conn.nmodes; i++) {
		const struct drm_display_mode *m = &w->modes[i];
		struct drm_mode_modeinfo u;
		int dup = -1;

		if (!edid_mode_is_plain(&w->info, m))
			continue;
		drm_mode_to_umode(&u, m);
		u.flags &= ~DRM_MODE_FLAG_PIC_AR_MASK;
		if (u.hdisplay == 0 || u.vdisplay == 0 || u.clock == 0)
			continue;
		for (j = 0; j < out->nmodes; j++) {
			if (drm_mode_equal(&out->modes[j], &u)) {
				dup = j;
				break;
			}
		}
		if (dup >= 0) {
			out->modes[dup].type |= u.type & DRM_MODE_TYPE_PREFERRED;
			if (i == 0 && dtd0_is_mode0)
				first_detailed = dup;
			continue;
		}
		if (out->nmodes >= DRM_EDID_MAX_MODES)
			continue;
		if (i == 0 && dtd0_is_mode0)
			first_detailed = out->nmodes;
		out->modes[out->nmodes++] = u;
	}

	/* One preferred mode: the first the EDID marks. */
	for (i = 0; i < out->nmodes; i++) {
		if (!(out->modes[i].type & DRM_MODE_TYPE_PREFERRED))
			continue;
		if (out->preferred < 0)
			out->preferred = i;
		else
			out->modes[i].type &= ~DRM_MODE_TYPE_PREFERRED;
	}
	/* None marked: an EDID before 1.4 still means its first detailed
	 * timing; else the largest mode. */
	if (out->preferred < 0 && base->revision < 4 && first_detailed >= 0)
		out->preferred = first_detailed;
	if (out->preferred < 0 && out->nmodes > 0) {
		int best = 0;
		for (i = 1; i < out->nmodes; i++) {
			uint32_t a = (uint32_t)out->modes[i].hdisplay *
				     out->modes[i].vdisplay;
			uint32_t b = (uint32_t)out->modes[best].hdisplay *
				     out->modes[best].vdisplay;
			if (a > b || (a == b && out->modes[i].vrefresh >
						       out->modes[best].vrefresh))
				best = i;
		}
		out->preferred = best;
	}
	if (out->preferred >= 0)
		out->modes[out->preferred].type |= DRM_MODE_TYPE_PREFERRED;

	drm_display_info_reset(&w->info);
	kfree(w);
	drm_edid_free(drm_edid);
	return 0;
}
