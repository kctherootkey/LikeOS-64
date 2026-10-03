// LikeOS -- display modes: naming, matching, validating, sorting and
// generating timings, and the standard timing tables (pure: the host tests
// compile it).
//
// A mode is the set of numbers the pixel clock and the sync generator are
// programmed with.  This file computes what follows from them (the name,
// the refresh rate, the values the CRTC is actually loaded with), compares
// them in the ways the core and the EDID code need, checks them for sanity,
// orders a connector's list the way clients expect (preferred first,
// biggest next), and derives timings from a bare geometry with the VESA CVT
// and GTF formulas or for an analog television standard.
//
// It also holds the timing tables a display that gives too few modes of
// its own is matched against: the VESA DMT list, the CEA-861 video codes
// televisions use, and the HDMI 1.4 4k codes.  Each table exists once; the
// EDID code and the drivers reach it through the accessors at the end.
//
// The functions on the client form (struct drm_mode_modeinfo) that the
// EDID parser and the i915 driver call -- drm_mode_cvt, drm_mode_gtf,
// drm_mode_finish, drm_mode_equal, drm_mode_table_get, drm_mode_cea_vic,
// drm_mode_in_range -- are at the very end, on top of the rest.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from the XFree86 Project's, Dave Airlie's, Intel's, Luc Verhaegen's and Andy Ritger's code: X11
// Portions Copyright (C) 1997-2003 by The XFree86 Project, Inc.
// Portions Copyright (C) 2007 Dave Airlie <airlied@linux.ie>
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2005-2006 Luc Verhaegen
// Portions Copyright (C) 2001 Andy Ritger
// SPDX-License-Identifier for the portions derived from Luc Verhaegen's, Intel's, Red Hat's and Dennis Munsie's code: MIT
// Portions Copyright (C) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2010 Red Hat, Inc.
// Portions Copyright (C) 2006 Dennis Munsie

#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/gpu_util.h>

#ifdef __LIKEOS_KERNEL__
#include <kernel/io/console.h>
#define modes_warn(...) kprintf("[drm] " __VA_ARGS__)
#else
#define modes_warn(...) do { } while (0)
#endif

/* Debug-level messages (refused modes, the steps of the analog television
 * computation, drm_mode_debug_printmodeline): off by default, they would
 * fill the log on every probe. */
#define DRM_MODES_DEBUG 0

#if DRM_MODES_DEBUG
#define modes_dbg(...) modes_warn(__VA_ARGS__)
#else
#define modes_dbg(...) do { } while (0)
#endif

#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000UL
#endif

/* The pixel period in picoseconds: clocks that differ only by rounding
 * (148500 and 148352 do not; 25175 and 25174 do) compare equal this way. */
#define KHZ2PICOS(a) (1000000000UL / (a))

static void mode_zero(struct drm_display_mode *m)
{
	unsigned int i;

	for (i = 0; i < sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;
}

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

/* ---- text ------------------------------------------------------------------ */

/* A bounded string being built: what does not fit is dropped, the buffer
 * is always terminated, len counts what was written. */
struct mode_buf {
	char *p;
	unsigned int cap;
	unsigned int len;
};

static void mb_putc(struct mode_buf *b, char c)
{
	if (b->len + 1 < b->cap) {
		b->p[b->len++] = c;
		b->p[b->len] = 0;
	}
}

static void mb_puts(struct mode_buf *b, const char *s)
{
	while (*s)
		mb_putc(b, *s++);
}

static void mb_putu(struct mode_buf *b, unsigned long long v, unsigned int base)
{
	char tmp[24];
	unsigned int n = 0;

	do {
		unsigned int d = (unsigned int)(v % base);

		tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v /= base;
	} while (v && n < sizeof(tmp));
	while (n)
		mb_putc(b, tmp[--n]);
}

static void mb_putd(struct mode_buf *b, long long v)
{
	if (v < 0) {
		mb_putc(b, '-');
		mb_putu(b, (unsigned long long)(-(v + 1)) + 1, 10);
	} else {
		mb_putu(b, (unsigned long long)v, 10);
	}
}

static void mb_init(struct mode_buf *b, char *p, unsigned int cap)
{
	b->p = p;
	b->cap = cap;
	b->len = 0;
	if (cap)
		p[0] = 0;
}

/**
 * drm_mode_set_name - set the name on a mode
 *
 * "<hdisplay>x<vdisplay>", with an "i" for an interlaced mode.
 */
void drm_mode_set_name(struct drm_display_mode *mode)
{
	bool interlaced = !!(mode->flags & DRM_MODE_FLAG_INTERLACE);
	struct mode_buf b;

	mb_init(&b, mode->name, DRM_DISPLAY_MODE_LEN);
	mb_putd(&b, mode->hdisplay);
	mb_putc(&b, 'x');
	mb_putd(&b, mode->vdisplay);
	if (interlaced)
		mb_putc(&b, 'i');
}

int drm_mode_snprint(char *buf, unsigned int len,
		     const struct drm_display_mode *mode)
{
	struct mode_buf b;

	if (!len)
		return 0;
	/* DRM_MODE_FMT, spelled out */
	mb_init(&b, buf, len);
	mb_putc(&b, '"');
	mb_puts(&b, mode->name);
	mb_puts(&b, "\": ");
	mb_putd(&b, drm_mode_vrefresh(mode));
	mb_putc(&b, ' ');
	mb_putd(&b, mode->clock);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->hdisplay);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->hsync_start);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->hsync_end);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->htotal);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->vdisplay);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->vsync_start);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->vsync_end);
	mb_putc(&b, ' ');
	mb_putd(&b, mode->vtotal);
	mb_puts(&b, " 0x");
	mb_putu(&b, mode->type, 16);
	mb_puts(&b, " 0x");
	mb_putu(&b, mode->flags, 16);
	return (int)b.len;
}

/**
 * drm_mode_debug_printmodeline - print a mode to the log (debug level)
 */
void drm_mode_debug_printmodeline(const struct drm_display_mode *mode)
{
	modes_dbg("Modeline " DRM_MODE_FMT "\n", DRM_MODE_ARG(mode));
	(void)mode;
}

/* ---- analog television ----------------------------------------------------- */

enum drm_mode_analog {
	DRM_MODE_ANALOG_NTSC, /* 525 lines, 60Hz */
	DRM_MODE_ANALOG_PAL, /* 625 lines, 50Hz */
};

/*
 * The line and blanking durations of the two systems (BT.601 and the
 * broadcast standards), with the tolerance each allows.
 */
#define NTSC_LINE_DURATION_NS		63556U
#define NTSC_LINES_NUMBER		525

#define NTSC_HBLK_DURATION_TYP_NS	10900U
#define NTSC_HBLK_DURATION_MIN_NS	(NTSC_HBLK_DURATION_TYP_NS - 200)
#define NTSC_HBLK_DURATION_MAX_NS	(NTSC_HBLK_DURATION_TYP_NS + 200)

#define NTSC_HACT_DURATION_TYP_NS	(NTSC_LINE_DURATION_NS - NTSC_HBLK_DURATION_TYP_NS)
#define NTSC_HACT_DURATION_MIN_NS	(NTSC_LINE_DURATION_NS - NTSC_HBLK_DURATION_MAX_NS)
#define NTSC_HACT_DURATION_MAX_NS	(NTSC_LINE_DURATION_NS - NTSC_HBLK_DURATION_MIN_NS)

#define NTSC_HFP_DURATION_TYP_NS	1500
#define NTSC_HFP_DURATION_MIN_NS	1270
#define NTSC_HFP_DURATION_MAX_NS	2220

#define NTSC_HSLEN_DURATION_TYP_NS	4700
#define NTSC_HSLEN_DURATION_MIN_NS	(NTSC_HSLEN_DURATION_TYP_NS - 100)
#define NTSC_HSLEN_DURATION_MAX_NS	(NTSC_HSLEN_DURATION_TYP_NS + 100)

#define NTSC_HBP_DURATION_TYP_NS	4700

/*
 * No tolerance is published for the back porch; the sync length's is
 * used.
 */
#define NTSC_HBP_DURATION_MIN_NS	(NTSC_HBP_DURATION_TYP_NS - 100)
#define NTSC_HBP_DURATION_MAX_NS	(NTSC_HBP_DURATION_TYP_NS + 100)

#define PAL_LINE_DURATION_NS		64000U
#define PAL_LINES_NUMBER		625

#define PAL_HACT_DURATION_TYP_NS	51950U
#define PAL_HACT_DURATION_MIN_NS	(PAL_HACT_DURATION_TYP_NS - 100)
#define PAL_HACT_DURATION_MAX_NS	(PAL_HACT_DURATION_TYP_NS + 400)

#define PAL_HBLK_DURATION_TYP_NS	(PAL_LINE_DURATION_NS - PAL_HACT_DURATION_TYP_NS)
#define PAL_HBLK_DURATION_MIN_NS	(PAL_LINE_DURATION_NS - PAL_HACT_DURATION_MAX_NS)
#define PAL_HBLK_DURATION_MAX_NS	(PAL_LINE_DURATION_NS - PAL_HACT_DURATION_MIN_NS)

#define PAL_HFP_DURATION_TYP_NS		1650
#define PAL_HFP_DURATION_MIN_NS		(PAL_HFP_DURATION_TYP_NS - 100)
#define PAL_HFP_DURATION_MAX_NS		(PAL_HFP_DURATION_TYP_NS + 400)

#define PAL_HSLEN_DURATION_TYP_NS	4700
#define PAL_HSLEN_DURATION_MIN_NS	(PAL_HSLEN_DURATION_TYP_NS - 200)
#define PAL_HSLEN_DURATION_MAX_NS	(PAL_HSLEN_DURATION_TYP_NS + 200)

#define PAL_HBP_DURATION_TYP_NS		5700
#define PAL_HBP_DURATION_MIN_NS		(PAL_HBP_DURATION_TYP_NS - 200)
#define PAL_HBP_DURATION_MAX_NS		(PAL_HBP_DURATION_TYP_NS + 200)

struct analog_param_field {
	unsigned int even, odd;
};

#define PARAM_FIELD(_odd, _even)		\
	{ .even = _even, .odd = _odd }

struct analog_param_range {
	unsigned int	min, typ, max;
};

#define PARAM_RANGE(_min, _typ, _max)		\
	{ .min = _min, .typ = _typ, .max = _max }

struct analog_parameters {
	unsigned int			num_lines;
	unsigned int			line_duration_ns;

	struct analog_param_range	hact_ns;
	struct analog_param_range	hfp_ns;
	struct analog_param_range	hslen_ns;
	struct analog_param_range	hbp_ns;
	struct analog_param_range	hblk_ns;

	unsigned int			bt601_hfp;

	struct analog_param_field	vfp_lines;
	struct analog_param_field	vslen_lines;
	struct analog_param_field	vbp_lines;
};

#define TV_MODE_PARAMETER(_mode, _lines, _line_dur, _hact, _hfp,	\
			  _hslen, _hbp, _hblk, _bt601_hfp, _vfp,	\
			  _vslen, _vbp)					\
	[_mode] = {							\
		.num_lines = _lines,					\
		.line_duration_ns = _line_dur,				\
		.hact_ns = _hact,					\
		.hfp_ns = _hfp,						\
		.hslen_ns = _hslen,					\
		.hbp_ns = _hbp,						\
		.hblk_ns = _hblk,					\
		.bt601_hfp = _bt601_hfp,				\
		.vfp_lines = _vfp,					\
		.vslen_lines = _vslen,					\
		.vbp_lines = _vbp,					\
	}

static const struct analog_parameters tv_modes_parameters[] = {
	TV_MODE_PARAMETER(DRM_MODE_ANALOG_NTSC,
			  NTSC_LINES_NUMBER,
			  NTSC_LINE_DURATION_NS,
			  PARAM_RANGE(NTSC_HACT_DURATION_MIN_NS,
				      NTSC_HACT_DURATION_TYP_NS,
				      NTSC_HACT_DURATION_MAX_NS),
			  PARAM_RANGE(NTSC_HFP_DURATION_MIN_NS,
				      NTSC_HFP_DURATION_TYP_NS,
				      NTSC_HFP_DURATION_MAX_NS),
			  PARAM_RANGE(NTSC_HSLEN_DURATION_MIN_NS,
				      NTSC_HSLEN_DURATION_TYP_NS,
				      NTSC_HSLEN_DURATION_MAX_NS),
			  PARAM_RANGE(NTSC_HBP_DURATION_MIN_NS,
				      NTSC_HBP_DURATION_TYP_NS,
				      NTSC_HBP_DURATION_MAX_NS),
			  PARAM_RANGE(NTSC_HBLK_DURATION_MIN_NS,
				      NTSC_HBLK_DURATION_TYP_NS,
				      NTSC_HBLK_DURATION_MAX_NS),
			  16,
			  PARAM_FIELD(3, 3),
			  PARAM_FIELD(3, 3),
			  PARAM_FIELD(16, 17)),
	TV_MODE_PARAMETER(DRM_MODE_ANALOG_PAL,
			  PAL_LINES_NUMBER,
			  PAL_LINE_DURATION_NS,
			  PARAM_RANGE(PAL_HACT_DURATION_MIN_NS,
				      PAL_HACT_DURATION_TYP_NS,
				      PAL_HACT_DURATION_MAX_NS),
			  PARAM_RANGE(PAL_HFP_DURATION_MIN_NS,
				      PAL_HFP_DURATION_TYP_NS,
				      PAL_HFP_DURATION_MAX_NS),
			  PARAM_RANGE(PAL_HSLEN_DURATION_MIN_NS,
				      PAL_HSLEN_DURATION_TYP_NS,
				      PAL_HSLEN_DURATION_MAX_NS),
			  PARAM_RANGE(PAL_HBP_DURATION_MIN_NS,
				      PAL_HBP_DURATION_TYP_NS,
				      PAL_HBP_DURATION_MAX_NS),
			  PARAM_RANGE(PAL_HBLK_DURATION_MIN_NS,
				      PAL_HBLK_DURATION_TYP_NS,
				      PAL_HBLK_DURATION_MAX_NS),
			  12,

			  /*
			   * The front porch is actually 6 short sync
			   * pulses for the even field, and 5 for the
			   * odd field. Each sync takes half a life so
			   * the odd field front porch is shorter by
			   * half a line.
			   *
			   * In progressive, we're supposed to use 6
			   * pulses, so we're fine there
			   */
			  PARAM_FIELD(3, 2),

			  /*
			   * The vsync length is 5 long sync pulses,
			   * each field taking half a line. We're
			   * shorter for both fields by half a line.
			   *
			   * In progressive, we're supposed to use 5
			   * pulses, so we're off by half
			   * a line.
			   *
			   * In interlace, we're now off by half a line
			   * for the even field and one line for the odd
			   * field.
			   */
			  PARAM_FIELD(3, 3),

			  /*
			   * The back porch starts with post-equalizing
			   * pulses, consisting in 5 short sync pulses
			   * for the even field, 4 for the odd field. In
			   * progressive, it's 5 short syncs.
			   *
			   * In progressive, we thus have 2.5 lines,
			   * plus the 0.5 line we were missing
			   * previously, so we should use 3 lines.
			   *
			   * In interlace, the even field is in the
			   * exact same case than progressive. For the
			   * odd field, we should be using 2 lines but
			   * we're one line short, so we'll make up for
			   * it here by using 3.
			   *
			   * The entire blanking area is supposed to
			   * take 25 lines, so we also need to account
			   * for the rest of the blanking area that
			   * can't be in either the front porch or sync
			   * period.
			   */
			  PARAM_FIELD(19, 20)),
};

static int fill_analog_mode(struct drm_display_mode *mode,
			    const struct analog_parameters *params,
			    unsigned long pixel_clock_hz,
			    unsigned int hactive,
			    unsigned int vactive,
			    bool interlace)
{
	unsigned long pixel_duration_ns = NSEC_PER_SEC / pixel_clock_hz;
	unsigned int htotal, vtotal;
	unsigned int max_hact, hact_duration_ns;
	unsigned int hblk, hblk_duration_ns;
	unsigned int hfp, hfp_duration_ns;
	unsigned int hslen, hslen_duration_ns;
	unsigned int hbp, hbp_duration_ns;
	unsigned int porches, porches_duration_ns;
	unsigned int vfp, vfp_min;
	unsigned int vbp, vbp_min;
	unsigned int vslen;
	bool bt601 = false;
	int porches_rem;
	u64 result;

	modes_dbg("Generating a %ux%u%c, %u-line mode with a %u kHz clock\n",
		  hactive, vactive, interlace ? 'i' : 'p', params->num_lines,
		  (unsigned int)(pixel_clock_hz / 1000));

	/* a clock above 1 GHz has a pixel shorter than a nanosecond: no
	 * television timing can be built from it */
	if (!pixel_duration_ns)
		return -EINVAL;

	max_hact = params->hact_ns.max / pixel_duration_ns;
	if (pixel_clock_hz == 13500000 && hactive > max_hact && hactive <= 720) {
		modes_dbg("Trying to generate a BT.601 mode. Disabling checks.\n");
		bt601 = true;
	}

	/*
	 * Our pixel duration is going to be round down by the division,
	 * so rounding up is probably going to introduce even more
	 * deviation.
	 */
	result = (u64)params->line_duration_ns * pixel_clock_hz;
	result /= NSEC_PER_SEC;
	htotal = (unsigned int)result;

	modes_dbg("Total Horizontal Number of Pixels: %u\n", htotal);

	hact_duration_ns = hactive * pixel_duration_ns;
	if (!bt601 &&
	    (hact_duration_ns < params->hact_ns.min ||
	     hact_duration_ns > params->hact_ns.max)) {
		modes_warn("Invalid horizontal active area duration: %uns (min: %u, max %u)\n",
			   hact_duration_ns, params->hact_ns.min, params->hact_ns.max);
		return -EINVAL;
	}

	hblk = htotal - hactive;
	modes_dbg("Horizontal Blanking Period: %u\n", hblk);

	hblk_duration_ns = hblk * pixel_duration_ns;
	if (!bt601 &&
	    (hblk_duration_ns < params->hblk_ns.min ||
	     hblk_duration_ns > params->hblk_ns.max)) {
		modes_warn("Invalid horizontal blanking duration: %uns (min: %u, max %u)\n",
			   hblk_duration_ns, params->hblk_ns.min, params->hblk_ns.max);
		return -EINVAL;
	}

	hslen = DIV_ROUND_UP(params->hslen_ns.typ, pixel_duration_ns);
	modes_dbg("Horizontal Sync Period: %u\n", hslen);

	hslen_duration_ns = hslen * pixel_duration_ns;
	if (!bt601 &&
	    (hslen_duration_ns < params->hslen_ns.min ||
	     hslen_duration_ns > params->hslen_ns.max)) {
		modes_warn("Invalid horizontal sync duration: %uns (min: %u, max %u)\n",
			   hslen_duration_ns, params->hslen_ns.min, params->hslen_ns.max);
		return -EINVAL;
	}

	porches = hblk - hslen;
	modes_dbg("Remaining horizontal pixels for both porches: %u\n", porches);

	porches_duration_ns = porches * pixel_duration_ns;
	if (!bt601 &&
	    (porches_duration_ns > (params->hfp_ns.max + params->hbp_ns.max) ||
	     porches_duration_ns < (params->hfp_ns.min + params->hbp_ns.min))) {
		modes_warn("Invalid horizontal porches duration: %uns\n",
			   porches_duration_ns);
		return -EINVAL;
	}

	if (bt601) {
		hfp = params->bt601_hfp;
	} else {
		unsigned int hfp_min = DIV_ROUND_UP(params->hfp_ns.min,
						    pixel_duration_ns);
		unsigned int hbp_min = DIV_ROUND_UP(params->hbp_ns.min,
						    pixel_duration_ns);
		int porches_rem = porches - hfp_min - hbp_min;

		hfp = hfp_min + DIV_ROUND_UP(porches_rem, 2);
	}

	modes_dbg("Horizontal Front Porch: %u\n", hfp);

	hfp_duration_ns = hfp * pixel_duration_ns;
	if (!bt601 &&
	    (hfp_duration_ns < params->hfp_ns.min ||
	     hfp_duration_ns > params->hfp_ns.max)) {
		modes_warn("Invalid horizontal front porch duration: %uns (min: %u, max %u)\n",
			   hfp_duration_ns, params->hfp_ns.min, params->hfp_ns.max);
		return -EINVAL;
	}

	hbp = porches - hfp;
	modes_dbg("Horizontal Back Porch: %u\n", hbp);

	hbp_duration_ns = hbp * pixel_duration_ns;
	if (!bt601 &&
	    (hbp_duration_ns < params->hbp_ns.min ||
	     hbp_duration_ns > params->hbp_ns.max)) {
		modes_warn("Invalid horizontal back porch duration: %uns (min: %u, max %u)\n",
			   hbp_duration_ns, params->hbp_ns.min, params->hbp_ns.max);
		return -EINVAL;
	}

	if (htotal != (hactive + hfp + hslen + hbp))
		return -EINVAL;

	mode->clock = (int)(pixel_clock_hz / 1000);
	mode->hdisplay = (uint16_t)hactive;
	mode->hsync_start = (uint16_t)(mode->hdisplay + hfp);
	mode->hsync_end = (uint16_t)(mode->hsync_start + hslen);
	mode->htotal = (uint16_t)(mode->hsync_end + hbp);

	if (interlace) {
		vfp_min = params->vfp_lines.even + params->vfp_lines.odd;
		vbp_min = params->vbp_lines.even + params->vbp_lines.odd;
		vslen = params->vslen_lines.even + params->vslen_lines.odd;
	} else {
		/*
		 * By convention, NTSC (aka 525/60) systems start with
		 * the even field, but PAL (aka 625/50) systems start
		 * with the odd one.
		 *
		 * PAL systems also have asymmetric timings between the
		 * even and odd field, while NTSC is symmetric.
		 *
		 * Moreover, if we want to create a progressive mode for
		 * PAL, we need to use the odd field timings.
		 *
		 * Since odd == even for NTSC, we can just use the odd
		 * one all the time to simplify the code a bit.
		 */
		vfp_min = params->vfp_lines.odd;
		vbp_min = params->vbp_lines.odd;
		vslen = params->vslen_lines.odd;
	}

	modes_dbg("Vertical Sync Period: %u\n", vslen);

	porches = params->num_lines - vactive - vslen;
	modes_dbg("Remaining vertical pixels for both porches: %u\n", porches);

	porches_rem = porches - vfp_min - vbp_min;
	vfp = vfp_min + (porches_rem / 2);
	modes_dbg("Vertical Front Porch: %u\n", vfp);

	vbp = porches - vfp;
	modes_dbg("Vertical Back Porch: %u\n", vbp);

	vtotal = vactive + vfp + vslen + vbp;
	if (params->num_lines != vtotal) {
		modes_warn("Invalid vertical total: %upx (expected %upx)\n",
			   vtotal, params->num_lines);
		return -EINVAL;
	}

	mode->vdisplay = (uint16_t)vactive;
	mode->vsync_start = (uint16_t)(mode->vdisplay + vfp);
	mode->vsync_end = (uint16_t)(mode->vsync_start + vslen);
	mode->vtotal = (uint16_t)(mode->vsync_end + vbp);

	if (mode->vtotal != params->num_lines)
		return -EINVAL;

	mode->type = DRM_MODE_TYPE_DRIVER;
	mode->flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC;
	if (interlace)
		mode->flags |= DRM_MODE_FLAG_INTERLACE;

	drm_mode_set_name(mode);

	modes_dbg("Generated mode " DRM_MODE_FMT "\n", DRM_MODE_ARG(mode));

	return 0;
}

/**
 * drm_analog_tv_mode - the display mode of an analog TV standard
 * @mode: filled in (zeroed first)
 * @tv_mode: DRM_MODE_TV_MODE_*; MONOCHROME gives a 625-line mode
 * @pixel_clock_hz: pixel clock, Hz
 * @hdisplay, @vdisplay: active size
 * @interlace: an interlaced mode
 *
 * At the BT.601 clock (13.5 MHz) an active width up to 720 is accepted
 * beyond the standard's active-line tolerance, with BT.601's front porch.
 *
 * Returns 0, or -EINVAL when the standard's line and blanking durations
 * cannot be met at that clock and size.
 */
int drm_analog_tv_mode(struct drm_display_mode *mode,
		       enum drm_connector_tv_mode tv_mode,
		       unsigned long pixel_clock_hz, unsigned int hdisplay,
		       unsigned int vdisplay, bool interlace)
{
	enum drm_mode_analog analog;

	mode_zero(mode);
	if (!pixel_clock_hz)
		return -EINVAL;

	switch (tv_mode) {
	case DRM_MODE_TV_MODE_NTSC:
		fallthrough;
	case DRM_MODE_TV_MODE_NTSC_443:
		fallthrough;
	case DRM_MODE_TV_MODE_NTSC_J:
		fallthrough;
	case DRM_MODE_TV_MODE_PAL_M:
		analog = DRM_MODE_ANALOG_NTSC;
		break;

	case DRM_MODE_TV_MODE_PAL:
		fallthrough;
	case DRM_MODE_TV_MODE_PAL_N:
		fallthrough;
	case DRM_MODE_TV_MODE_SECAM:
		fallthrough;
	case DRM_MODE_TV_MODE_MONOCHROME:
		analog = DRM_MODE_ANALOG_PAL;
		break;

	default:
		return -EINVAL;
	}

	return fill_analog_mode(mode, &tv_modes_parameters[analog],
				pixel_clock_hz, hdisplay, vdisplay, interlace);
}

/* ---- CVT and GTF ----------------------------------------------------------- */

/**
 * drm_cvt_mode - a modeline from the CVT algorithm
 * @mode: filled in (zeroed first)
 * @hdisplay, @vdisplay: the geometry
 * @vrefresh: refresh rate in Hz (0: 60)
 * @reduced: reduced blanking (v1), for digital sinks
 * @interlaced: an interlaced mode
 * @margins: add the standard 1.8% borders
 *
 * The VESA Coordinated Video Timing formula in integer arithmetic.  The
 * pixel clock is not rounded down to CVT's 250 kHz step (the clock the
 * formula yields is what is filled in).
 *
 * Returns 0, or -EINVAL for a zero geometry or one whose horizontal period
 * comes out as zero.
 */
int drm_cvt_mode(struct drm_display_mode *mode, int hdisplay, int vdisplay,
		 int vrefresh, bool reduced, bool interlaced, bool margins)
{
#define HV_FACTOR			1000
	/* 1) top/bottom margin size (% of height) - default: 1.8, */
#define	CVT_MARGIN_PERCENTAGE		18
	/* 2) character cell horizontal granularity (pixels) - default 8 */
#define	CVT_H_GRANULARITY		8
	/* 3) Minimum vertical porch (lines) - default 3 */
#define	CVT_MIN_V_PORCH			3
	/* 4) Minimum number of vertical back porch lines - default 6 */
#define	CVT_MIN_V_BPORCH		6
	/* Pixel Clock step (kHz) */
#define CVT_CLOCK_STEP			250
	unsigned int vfieldrate, hperiod;
	int hdisplay_rnd, hmargin, vdisplay_rnd, vmargin, vsync;
	int interlace;
	u64 tmp;

	mode_zero(mode);
	if (!hdisplay || !vdisplay)
		return -EINVAL;

	/* the CVT default refresh rate is 60Hz */
	if (!vrefresh)
		vrefresh = 60;

	/* the required field fresh rate */
	if (interlaced)
		vfieldrate = vrefresh * 2;
	else
		vfieldrate = vrefresh;

	/* horizontal pixels */
	hdisplay_rnd = hdisplay - (hdisplay % CVT_H_GRANULARITY);

	/* determine the left&right borders */
	hmargin = 0;
	if (margins) {
		hmargin = hdisplay_rnd * CVT_MARGIN_PERCENTAGE / 1000;
		hmargin -= hmargin % CVT_H_GRANULARITY;
	}
	/* find the total active pixels */
	mode->hdisplay = (uint16_t)(hdisplay_rnd + 2 * hmargin);

	/* find the number of lines per field */
	if (interlaced)
		vdisplay_rnd = vdisplay / 2;
	else
		vdisplay_rnd = vdisplay;

	/* find the top & bottom borders */
	vmargin = 0;
	if (margins)
		vmargin = vdisplay_rnd * CVT_MARGIN_PERCENTAGE / 1000;

	mode->vdisplay = (uint16_t)(vdisplay + 2 * vmargin);

	/* Interlaced */
	if (interlaced)
		interlace = 1;
	else
		interlace = 0;

	/* Determine VSync Width from aspect ratio */
	if (!(vdisplay % 3) && ((vdisplay * 4 / 3) == hdisplay))
		vsync = 4;
	else if (!(vdisplay % 9) && ((vdisplay * 16 / 9) == hdisplay))
		vsync = 5;
	else if (!(vdisplay % 10) && ((vdisplay * 16 / 10) == hdisplay))
		vsync = 6;
	else if (!(vdisplay % 4) && ((vdisplay * 5 / 4) == hdisplay))
		vsync = 7;
	else if (!(vdisplay % 9) && ((vdisplay * 15 / 9) == hdisplay))
		vsync = 7;
	else /* custom */
		vsync = 10;

	if (!reduced) {
		/* simplify the GTF calculation */
		/* 4) Minimum time of vertical sync + back porch interval (µs)
		 * default 550.0
		 */
		int tmp1, tmp2;
#define CVT_MIN_VSYNC_BP	550
		/* 3) Nominal HSync width (% of line period) - default 8 */
#define CVT_HSYNC_PERCENTAGE	8
		unsigned int hblank_percentage;
		int vsyncandback_porch, __maybe_unused vback_porch, hblank;
		unsigned int div;

		/* estimated the horizontal period */
		tmp1 = HV_FACTOR * 1000000  -
				CVT_MIN_VSYNC_BP * HV_FACTOR * vfieldrate;
		tmp2 = (vdisplay_rnd + 2 * vmargin + CVT_MIN_V_PORCH) * 2 +
				interlace;
		div = (unsigned int)tmp2 * vfieldrate;
		if (!div)
			return -EINVAL;
		hperiod = tmp1 * 2 / div;
		if (!hperiod)
			return -EINVAL;

		tmp1 = CVT_MIN_VSYNC_BP * HV_FACTOR / hperiod + 1;
		/* 9. Find number of lines in sync + backporch */
		if (tmp1 < (vsync + CVT_MIN_V_PORCH))
			vsyncandback_porch = vsync + CVT_MIN_V_PORCH;
		else
			vsyncandback_porch = tmp1;
		/* 10. Find number of lines in back porch */
		vback_porch = vsyncandback_porch - vsync;
		mode->vtotal = (uint16_t)(vdisplay_rnd + 2 * vmargin +
				vsyncandback_porch + CVT_MIN_V_PORCH);
		/* 5) Definition of Horizontal blanking time limitation */
		/* Gradient (%/kHz) - default 600 */
#define CVT_M_FACTOR	600
		/* Offset (%) - default 40 */
#define CVT_C_FACTOR	40
		/* Blanking time scaling factor - default 128 */
#define CVT_K_FACTOR	128
		/* Scaling factor weighting - default 20 */
#define CVT_J_FACTOR	20
#define CVT_M_PRIME	(CVT_M_FACTOR * CVT_K_FACTOR / 256)
#define CVT_C_PRIME	((CVT_C_FACTOR - CVT_J_FACTOR) * CVT_K_FACTOR / 256 + \
			 CVT_J_FACTOR)
		/* 12. Find ideal blanking duty cycle from formula */
		hblank_percentage = CVT_C_PRIME * HV_FACTOR - CVT_M_PRIME *
					hperiod / 1000;
		/* 13. Blanking time */
		if (hblank_percentage < 20 * HV_FACTOR)
			hblank_percentage = 20 * HV_FACTOR;
		if (hblank_percentage == 100 * HV_FACTOR)
			return -EINVAL;
		hblank = mode->hdisplay * hblank_percentage /
			 (100 * HV_FACTOR - hblank_percentage);
		hblank -= hblank % (2 * CVT_H_GRANULARITY);
		/* 14. find the total pixels per line */
		mode->htotal = (uint16_t)(mode->hdisplay + hblank);
		mode->hsync_end = (uint16_t)(mode->hdisplay + hblank / 2);
		mode->hsync_start = (uint16_t)(mode->hsync_end -
			(mode->htotal * CVT_HSYNC_PERCENTAGE) / 100);
		mode->hsync_start += CVT_H_GRANULARITY -
			mode->hsync_start % CVT_H_GRANULARITY;
		/* fill the Vsync values */
		mode->vsync_start = (uint16_t)(mode->vdisplay + CVT_MIN_V_PORCH);
		mode->vsync_end = (uint16_t)(mode->vsync_start + vsync);
	} else {
		/* Reduced blanking */
		/* Minimum vertical blanking interval time (µs)- default 460 */
#define CVT_RB_MIN_VBLANK	460
		/* Fixed number of clocks for horizontal sync */
#define CVT_RB_H_SYNC		32
		/* Fixed number of clocks for horizontal blanking */
#define CVT_RB_H_BLANK		160
		/* Fixed number of lines for vertical front porch - default 3*/
#define CVT_RB_VFPORCH		3
		int vbilines;
		int tmp1, tmp2;
		unsigned int div;

		/* 8. Estimate Horizontal period. */
		tmp1 = HV_FACTOR * 1000000 -
			CVT_RB_MIN_VBLANK * HV_FACTOR * vfieldrate;
		tmp2 = vdisplay_rnd + 2 * vmargin;
		div = (unsigned int)tmp2 * vfieldrate;
		if (!div)
			return -EINVAL;
		hperiod = tmp1 / div;
		if (!hperiod)
			return -EINVAL;
		/* 9. Find number of lines in vertical blanking */
		vbilines = CVT_RB_MIN_VBLANK * HV_FACTOR / hperiod + 1;
		/* 10. Check if vertical blanking is sufficient */
		if (vbilines < (CVT_RB_VFPORCH + vsync + CVT_MIN_V_BPORCH))
			vbilines = CVT_RB_VFPORCH + vsync + CVT_MIN_V_BPORCH;
		/* 11. Find total number of lines in vertical field */
		mode->vtotal = (uint16_t)(vdisplay_rnd + 2 * vmargin + vbilines);
		/* 12. Find total number of pixels in a line */
		mode->htotal = (uint16_t)(mode->hdisplay + CVT_RB_H_BLANK);
		/* Fill in HSync values */
		mode->hsync_end = (uint16_t)(mode->hdisplay + CVT_RB_H_BLANK / 2);
		mode->hsync_start = (uint16_t)(mode->hsync_end - CVT_RB_H_SYNC);
		/* Fill in VSync values */
		mode->vsync_start = (uint16_t)(mode->vdisplay + CVT_RB_VFPORCH);
		mode->vsync_end = (uint16_t)(mode->vsync_start + vsync);
	}
	/* 15/13. Find pixel clock frequency (kHz) */
	tmp = mode->htotal; /* perform intermediate calcs in u64 */
	tmp *= HV_FACTOR * 1000;
	tmp /= hperiod;
	/* the clock field is still 0 here: no rounding to CVT_CLOCK_STEP
	 * takes place, the formula's clock is used as it comes */
	tmp -= mode->clock % CVT_CLOCK_STEP;
	mode->clock = (int)tmp;
	/* 18/16. Find actual vertical frame frequency */
	/* ignore - just set the mode flag for interlaced */
	if (interlaced) {
		mode->vtotal *= 2;
		mode->flags |= DRM_MODE_FLAG_INTERLACE;
	}
	/* Fill the mode line name */
	drm_mode_set_name(mode);
	if (reduced)
		mode->flags |= (DRM_MODE_FLAG_PHSYNC |
					DRM_MODE_FLAG_NVSYNC);
	else
		mode->flags |= (DRM_MODE_FLAG_PVSYNC |
					DRM_MODE_FLAG_NHSYNC);

	return 0;
}

/**
 * drm_gtf_mode_complex - a modeline from the full GTF algorithm
 * @mode: filled in (zeroed first)
 * @hdisplay, @vdisplay: the geometry
 * @vrefresh: refresh rate in Hz
 * @interlaced: an interlaced mode
 * @margins: add the standard 1.8% borders
 * @GTF_M, @GTF_2C, @GTF_K, @GTF_2J: the formula's parameters
 *
 * GTF feature blocks specify C and J in multiples of 0.5, so they are
 * passed in multiplied by two.  For a C of 40, pass in 80.
 *
 * Returns 0, or -EINVAL for a zero geometry or refresh, or parameters
 * that leave the formula a division by zero.
 */
int drm_gtf_mode_complex(struct drm_display_mode *mode, int hdisplay,
			 int vdisplay, int vrefresh, bool interlaced,
			 int margins, int GTF_M, int GTF_2C, int GTF_K,
			 int GTF_2J)
{	/* 1) top/bottom margin size (% of height) - default: 1.8, */
#define	GTF_MARGIN_PERCENTAGE		18
	/* 2) character cell horizontal granularity (pixels) - default 8 */
#define	GTF_CELL_GRAN			8
	/* 3) Minimum vertical porch (lines) - default 3 */
#define	GTF_MIN_V_PORCH			1
	/* width of vsync in lines */
#define V_SYNC_RQD			3
	/* width of hsync as % of total line */
#define H_SYNC_PERCENT			8
	/* min time of vsync + back porch (microsec) */
#define MIN_VSYNC_PLUS_BP		550
	/* C' and M' are part of the Blanking Duty Cycle computation */
#define GTF_C_PRIME	((((GTF_2C - GTF_2J) * GTF_K / 256) + GTF_2J) / 2)
#define GTF_M_PRIME	(GTF_K * GTF_M / 256)
	unsigned int hdisplay_rnd, vdisplay_rnd, vfieldrate_rqd;
	int top_margin, bottom_margin;
	int interlace;
	unsigned int hfreq_est;
	int vsync_plus_bp, __maybe_unused vback_porch;
	unsigned int vtotal_lines, __maybe_unused vfieldrate_est;
	unsigned int __maybe_unused hperiod;
	unsigned int vfield_rate, __maybe_unused vframe_rate;
	int left_margin, right_margin;
	unsigned int total_active_pixels, ideal_duty_cycle;
	unsigned int hblank, total_pixels, pixel_freq;
	int hsync, hfront_porch, vodd_front_porch_lines;
	unsigned int tmp1, tmp2;

	mode_zero(mode);
	if (!hdisplay || !vdisplay || vrefresh <= 0)
		return -EINVAL;

	/* 1. In order to give correct results, the number of horizontal
	 * pixels requested is first processed to ensure that it is divisible
	 * by the character size, by rounding it to the nearest character
	 * cell boundary:
	 */
	hdisplay_rnd = (hdisplay + GTF_CELL_GRAN / 2) / GTF_CELL_GRAN;
	hdisplay_rnd = hdisplay_rnd * GTF_CELL_GRAN;

	/* 2. If interlace is requested, the number of vertical lines assumed
	 * by the calculation must be halved, as the computation calculates
	 * the number of vertical lines per field.
	 */
	if (interlaced)
		vdisplay_rnd = vdisplay / 2;
	else
		vdisplay_rnd = vdisplay;

	/* 3. Find the frame rate required: */
	if (interlaced)
		vfieldrate_rqd = vrefresh * 2;
	else
		vfieldrate_rqd = vrefresh;

	/* 4. Find number of lines in Top margin: */
	top_margin = 0;
	if (margins)
		top_margin = (vdisplay_rnd * GTF_MARGIN_PERCENTAGE + 500) /
				1000;
	/* 5. Find number of lines in bottom margin: */
	bottom_margin = top_margin;

	/* 6. If interlace is required, then set variable interlace: */
	if (interlaced)
		interlace = 1;
	else
		interlace = 0;

	/* 7. Estimate the Horizontal frequency */
	{
		tmp1 = (1000000  - MIN_VSYNC_PLUS_BP * vfieldrate_rqd) / 500;
		tmp2 = (vdisplay_rnd + 2 * top_margin + GTF_MIN_V_PORCH) *
				2 + interlace;
		if (!tmp1)
			return -EINVAL;
		hfreq_est = (tmp2 * 1000 * vfieldrate_rqd) / tmp1;
	}
	if (!hfreq_est)
		return -EINVAL;

	/* 8. Find the number of lines in V sync + back porch */
	/* [V SYNC+BP] = RINT(([MIN VSYNC+BP] * hfreq_est / 1000000)) */
	vsync_plus_bp = MIN_VSYNC_PLUS_BP * hfreq_est / 1000;
	vsync_plus_bp = (vsync_plus_bp + 500) / 1000;
	/*  9. Find the number of lines in V back porch alone: */
	vback_porch = vsync_plus_bp - V_SYNC_RQD;
	/*  10. Find the total number of lines in Vertical field period: */
	vtotal_lines = vdisplay_rnd + top_margin + bottom_margin +
			vsync_plus_bp + GTF_MIN_V_PORCH;
	/*  11. Estimate the Vertical field frequency: */
	vfieldrate_est = hfreq_est / vtotal_lines;
	/*  12. Find the actual horizontal period: */
	if (vfieldrate_rqd * vtotal_lines == 0)
		return -EINVAL;
	hperiod = 1000000 / (vfieldrate_rqd * vtotal_lines);

	/*  13. Find the actual Vertical field frequency: */
	vfield_rate = hfreq_est / vtotal_lines;
	/*  14. Find the Vertical frame frequency: */
	if (interlaced)
		vframe_rate = vfield_rate / 2;
	else
		vframe_rate = vfield_rate;
	/*  15. Find number of pixels in left margin: */
	if (margins)
		left_margin = (hdisplay_rnd * GTF_MARGIN_PERCENTAGE + 500) /
				1000;
	else
		left_margin = 0;

	/* 16.Find number of pixels in right margin: */
	right_margin = left_margin;
	/* 17.Find total number of active pixels in image and left and right */
	total_active_pixels = hdisplay_rnd + left_margin + right_margin;
	/* 18.Find the ideal blanking duty cycle from blanking duty cycle */
	ideal_duty_cycle = GTF_C_PRIME * 1000 -
				(GTF_M_PRIME * 1000000 / hfreq_est);
	/* 19.Find the number of pixels in the blanking time to the nearest
	 * double character cell: */
	if (ideal_duty_cycle == 100000)
		return -EINVAL;
	hblank = total_active_pixels * ideal_duty_cycle /
			(100000 - ideal_duty_cycle);
	hblank = (hblank + GTF_CELL_GRAN) / (2 * GTF_CELL_GRAN);
	hblank = hblank * 2 * GTF_CELL_GRAN;
	/* 20.Find total number of pixels: */
	total_pixels = total_active_pixels + hblank;
	/* 21.Find pixel clock frequency: */
	pixel_freq = total_pixels * hfreq_est / 1000;
	/* Stage 1 is complete; of stage 2 only the few values the mode
	 * needs follow. */
	/* 17. Find the number of pixels in the horizontal sync period: */
	hsync = H_SYNC_PERCENT * total_pixels / 100;
	hsync = (hsync + GTF_CELL_GRAN / 2) / GTF_CELL_GRAN;
	hsync = hsync * GTF_CELL_GRAN;
	/* 18. Find the number of pixels in horizontal front porch period */
	hfront_porch = hblank / 2 - hsync;
	/*  36. Find the number of lines in the odd front porch period: */
	vodd_front_porch_lines = GTF_MIN_V_PORCH ;

	/* finally, pack the results in the mode struct */
	mode->hdisplay = (uint16_t)hdisplay_rnd;
	mode->hsync_start = (uint16_t)(hdisplay_rnd + hfront_porch);
	mode->hsync_end = (uint16_t)(mode->hsync_start + hsync);
	mode->htotal = (uint16_t)total_pixels;
	mode->vdisplay = (uint16_t)vdisplay_rnd;
	mode->vsync_start = (uint16_t)(vdisplay_rnd + vodd_front_porch_lines);
	mode->vsync_end = (uint16_t)(mode->vsync_start + V_SYNC_RQD);
	mode->vtotal = (uint16_t)vtotal_lines;

	mode->clock = (int)pixel_freq;

	if (interlaced) {
		mode->vtotal *= 2;
		mode->flags |= DRM_MODE_FLAG_INTERLACE;
	}

	drm_mode_set_name(mode);
	/* The polarities replace the flags, the interlace flag included: an
	 * interlaced GTF mode keeps its doubled vtotal and its "i" name but
	 * not the flag.  The EDID code only asks for progressive modes. */
	if (GTF_M == 600 && GTF_2C == 80 && GTF_K == 128 && GTF_2J == 40)
		mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
	else
		mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC;

	return 0;
}

/**
 * drm_gtf_mode - a modeline from the GTF algorithm, default parameters
 *
 * The Generalized Timing Formula with its standard parameters
 * M = 600, C = 40, K = 128, J = 20.
 */
int drm_gtf_mode(struct drm_display_mode *mode, int hdisplay, int vdisplay,
		 int vrefresh, bool interlaced, int margins)
{
	return drm_gtf_mode_complex(mode, hdisplay, vdisplay, vrefresh,
				    interlaced, margins,
				    600, 40 * 2, 128, 20 * 2);
}

/* ---- refresh, crtc values, copies ------------------------------------------ */

/**
 * drm_mode_vrefresh - the vertical refresh rate of a mode
 *
 * In Hz, rounded to the nearest integer; 0 for a mode without totals or
 * one whose numbers overflow 32 bits.
 */
int drm_mode_vrefresh(const struct drm_display_mode *mode)
{
	return drm_mode_vrefresh_calc(mode);
}

/**
 * drm_mode_get_hv_timing - the displayed width and height of a mode
 *
 * The height is doubled for a stereo frame-packing mode.
 */
void drm_mode_get_hv_timing(const struct drm_display_mode *mode,
			    int *hdisplay, int *vdisplay)
{
	struct drm_display_mode adjusted;

	drm_mode_init(&adjusted, mode);

	drm_mode_set_crtcinfo(&adjusted, CRTC_STEREO_DOUBLE_ONLY);
	*hdisplay = adjusted.crtc_hdisplay;
	*vdisplay = adjusted.crtc_vdisplay;
}

/**
 * drm_mode_set_crtcinfo - set the CRTC timing values of a mode
 * @p: mode
 * @adjust_flags: CRTC_* adjustments
 *
 * - CRTC_INTERLACE_HALVE_V halves the vertical timings of an interlaced
 *   mode (the CRTC counts lines per field).
 * - CRTC_STEREO_DOUBLE computes the timings for a buffer holding both
 *   eyes, where the layout needs it ("frame packing").
 * - CRTC_NO_DBLSCAN and CRTC_NO_VSCAN leave double scan and vscan > 1
 *   modes unadjusted.
 */
void drm_mode_set_crtcinfo(struct drm_display_mode *p, int adjust_flags)
{
	if (!p)
		return;

	p->crtc_clock = p->clock;
	p->crtc_hdisplay = p->hdisplay;
	p->crtc_hsync_start = p->hsync_start;
	p->crtc_hsync_end = p->hsync_end;
	p->crtc_htotal = p->htotal;
	p->crtc_hskew = p->hskew;
	p->crtc_vdisplay = p->vdisplay;
	p->crtc_vsync_start = p->vsync_start;
	p->crtc_vsync_end = p->vsync_end;
	p->crtc_vtotal = p->vtotal;

	if (p->flags & DRM_MODE_FLAG_INTERLACE) {
		if (adjust_flags & CRTC_INTERLACE_HALVE_V) {
			p->crtc_vdisplay /= 2;
			p->crtc_vsync_start /= 2;
			p->crtc_vsync_end /= 2;
			p->crtc_vtotal /= 2;
		}
	}

	if (!(adjust_flags & CRTC_NO_DBLSCAN)) {
		if (p->flags & DRM_MODE_FLAG_DBLSCAN) {
			p->crtc_vdisplay *= 2;
			p->crtc_vsync_start *= 2;
			p->crtc_vsync_end *= 2;
			p->crtc_vtotal *= 2;
		}
	}

	if (!(adjust_flags & CRTC_NO_VSCAN)) {
		if (p->vscan > 1) {
			p->crtc_vdisplay *= p->vscan;
			p->crtc_vsync_start *= p->vscan;
			p->crtc_vsync_end *= p->vscan;
			p->crtc_vtotal *= p->vscan;
		}
	}

	if (adjust_flags & CRTC_STEREO_DOUBLE) {
		unsigned int layout = p->flags & DRM_MODE_FLAG_3D_MASK;

		switch (layout) {
		case DRM_MODE_FLAG_3D_FRAME_PACKING:
			p->crtc_clock *= 2;
			p->crtc_vdisplay += p->crtc_vtotal;
			p->crtc_vsync_start += p->crtc_vtotal;
			p->crtc_vsync_end += p->crtc_vtotal;
			p->crtc_vtotal += p->crtc_vtotal;
			break;
		}
	}

	p->crtc_vblank_start = min(p->crtc_vsync_start, p->crtc_vdisplay);
	p->crtc_vblank_end = max(p->crtc_vsync_end, p->crtc_vtotal);
	p->crtc_hblank_start = min(p->crtc_hsync_start, p->crtc_hdisplay);
	p->crtc_hblank_end = max(p->crtc_hsync_end, p->crtc_htotal);
}

/**
 * drm_mode_copy - copy a mode
 */
void drm_mode_copy(struct drm_display_mode *dst, const struct drm_display_mode *src)
{
	*dst = *src;
}

/**
 * drm_mode_init - initialize a mode from another one
 *
 * For a destination that may hold stack garbage: it is cleared first.
 */
void drm_mode_init(struct drm_display_mode *dst, const struct drm_display_mode *src)
{
	mode_zero(dst);
	drm_mode_copy(dst, src);
}

/* ---- matching ---------------------------------------------------------------- */

static bool drm_mode_match_timings_vrr(const struct drm_display_mode *mode1,
				       const struct drm_display_mode *mode2)
{
	int mode1_vsync_start_offset = mode1->vtotal - mode1->vsync_start;
	int mode1_vsync_end_offset = mode1->vtotal - mode1->vsync_end;
	int mode2_vsync_start_offset = mode2->vtotal - mode2->vsync_start;
	int mode2_vsync_end_offset = mode2->vtotal - mode2->vsync_end;

	return mode1->hdisplay == mode2->hdisplay &&
		mode1->hsync_start == mode2->hsync_start &&
		mode1->hsync_end == mode2->hsync_end &&
		mode1->htotal == mode2->htotal &&
		mode1->hskew == mode2->hskew &&
		mode1->vdisplay == mode2->vdisplay &&
		mode1_vsync_start_offset == mode2_vsync_start_offset &&
		mode1_vsync_end_offset == mode2_vsync_end_offset &&
		mode1->vscan == mode2->vscan;
}

static bool drm_mode_match_timings(const struct drm_display_mode *mode1,
				   const struct drm_display_mode *mode2)
{
	return mode1->hdisplay == mode2->hdisplay &&
		mode1->hsync_start == mode2->hsync_start &&
		mode1->hsync_end == mode2->hsync_end &&
		mode1->htotal == mode2->htotal &&
		mode1->hskew == mode2->hskew &&
		mode1->vdisplay == mode2->vdisplay &&
		mode1->vsync_start == mode2->vsync_start &&
		mode1->vsync_end == mode2->vsync_end &&
		mode1->vtotal == mode2->vtotal &&
		mode1->vscan == mode2->vscan;
}

static bool drm_mode_match_clock(const struct drm_display_mode *mode1,
				  const struct drm_display_mode *mode2)
{
	/*
	 * compare the pixel periods, so that modes whose clocks were
	 * rounded differently still match
	 */
	if (mode1->clock && mode2->clock)
		return KHZ2PICOS(mode1->clock) == KHZ2PICOS(mode2->clock);
	else
		return mode1->clock == mode2->clock;
}

static bool drm_mode_match_flags(const struct drm_display_mode *mode1,
				 const struct drm_display_mode *mode2)
{
	return (mode1->flags & ~DRM_MODE_FLAG_3D_MASK) ==
		(mode2->flags & ~DRM_MODE_FLAG_3D_MASK);
}

static bool drm_mode_match_3d_flags(const struct drm_display_mode *mode1,
				    const struct drm_display_mode *mode2)
{
	return (mode1->flags & DRM_MODE_FLAG_3D_MASK) ==
		(mode2->flags & DRM_MODE_FLAG_3D_MASK);
}

static bool drm_mode_match_aspect_ratio(const struct drm_display_mode *mode1,
					const struct drm_display_mode *mode2)
{
	return mode1->picture_aspect_ratio == mode2->picture_aspect_ratio;
}

/**
 * drm_mode_match - test modes for (partial) equality
 * @match_flags: which parts need to match (DRM_MODE_MATCH_*)
 */
bool drm_mode_match(const struct drm_display_mode *mode1,
		    const struct drm_display_mode *mode2,
		    unsigned int match_flags)
{
	if (!mode1 && !mode2)
		return true;

	if (!mode1 || !mode2)
		return false;

	if (match_flags & DRM_MODE_MATCH_TIMINGS_VRR &&
	    !drm_mode_match_timings_vrr(mode1, mode2))
		return false;

	if (match_flags & DRM_MODE_MATCH_TIMINGS &&
	    !drm_mode_match_timings(mode1, mode2))
		return false;

	if (match_flags & DRM_MODE_MATCH_CLOCK &&
	    !drm_mode_match_clock(mode1, mode2))
		return false;

	if (match_flags & DRM_MODE_MATCH_FLAGS &&
	    !drm_mode_match_flags(mode1, mode2))
		return false;

	if (match_flags & DRM_MODE_MATCH_3D_FLAGS &&
	    !drm_mode_match_3d_flags(mode1, mode2))
		return false;

	if (match_flags & DRM_MODE_MATCH_ASPECT_RATIO &&
	    !drm_mode_match_aspect_ratio(mode1, mode2))
		return false;

	return true;
}

/**
 * drm_display_mode_equal - test modes for equality
 *
 * Timings, clock, flags, stereo layout and picture aspect ratio.
 */
bool drm_display_mode_equal(const struct drm_display_mode *mode1,
			    const struct drm_display_mode *mode2)
{
	return drm_mode_match(mode1, mode2,
			      DRM_MODE_MATCH_TIMINGS |
			      DRM_MODE_MATCH_CLOCK |
			      DRM_MODE_MATCH_FLAGS |
			      DRM_MODE_MATCH_3D_FLAGS|
			      DRM_MODE_MATCH_ASPECT_RATIO);
}

/**
 * drm_mode_equal_no_clocks - test modes for equality, clocks aside
 */
bool drm_mode_equal_no_clocks(const struct drm_display_mode *mode1,
			      const struct drm_display_mode *mode2)
{
	return drm_mode_match(mode1, mode2,
			      DRM_MODE_MATCH_TIMINGS |
			      DRM_MODE_MATCH_FLAGS |
			      DRM_MODE_MATCH_3D_FLAGS);
}

/**
 * drm_mode_equal_no_clocks_no_stereo - test modes for equality, clocks
 * and stereo layout aside
 */
bool drm_mode_equal_no_clocks_no_stereo(const struct drm_display_mode *mode1,
					const struct drm_display_mode *mode2)
{
	return drm_mode_match(mode1, mode2,
			      DRM_MODE_MATCH_TIMINGS |
			      DRM_MODE_MATCH_FLAGS);
}

/* ---- validation ---------------------------------------------------------------- */

enum drm_mode_status
drm_mode_validate_basic(const struct drm_display_mode *mode)
{
	if (mode->type & ~DRM_MODE_TYPE_ALL)
		return MODE_BAD;

	if (mode->flags & ~DRM_MODE_FLAG_ALL)
		return MODE_BAD;

	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) > DRM_MODE_FLAG_3D_MAX)
		return MODE_BAD;

	if (mode->clock == 0)
		return MODE_CLOCK_LOW;

	if (mode->hdisplay == 0 ||
	    mode->hsync_start < mode->hdisplay ||
	    mode->hsync_end < mode->hsync_start ||
	    mode->htotal < mode->hsync_end)
		return MODE_H_ILLEGAL;

	if (mode->vdisplay == 0 ||
	    mode->vsync_start < mode->vdisplay ||
	    mode->vsync_end < mode->vsync_start ||
	    mode->vtotal < mode->vsync_end)
		return MODE_V_ILLEGAL;

	return MODE_OK;
}

/**
 * drm_mode_validate_driver - make sure the mode is somewhat sane
 *
 * The basic checks.  The drivers' limits are per connector
 * (drm_driver.mode_valid); the connector code asks them.
 */
enum drm_mode_status
drm_mode_validate_driver(struct drm_device *dev,
			 const struct drm_display_mode *mode)
{
	(void)dev;
	return drm_mode_validate_basic(mode);
}

/**
 * drm_mode_validate_size - make sure a mode fits the size limits
 * @maxX, @maxY: largest width and height (0 or less: no limit)
 */
enum drm_mode_status
drm_mode_validate_size(const struct drm_display_mode *mode,
		       int maxX, int maxY)
{
	if (maxX > 0 && mode->hdisplay > maxX)
		return MODE_VIRTUAL_X;

	if (maxY > 0 && mode->vdisplay > maxY)
		return MODE_VIRTUAL_Y;

	return MODE_OK;
}

/**
 * drm_mode_validate_ycbcr420 - keep 4:2:0-only modes only when allowed
 * @info: the sink
 * @ycbcr_420_allowed: the source can send YCbCr 4:2:0
 */
enum drm_mode_status
drm_mode_validate_ycbcr420(const struct drm_display_mode *mode,
			   const struct drm_display_info *info,
			   bool ycbcr_420_allowed)
{
	if (!ycbcr_420_allowed &&
	    drm_mode_is_420_only(info, mode))
		return MODE_NO_420;

	return MODE_OK;
}

#define MODE_STATUS(status) [MODE_ ## status + 3] = #status

static const char * const drm_mode_status_names[] = {
	MODE_STATUS(OK),
	MODE_STATUS(HSYNC),
	MODE_STATUS(VSYNC),
	MODE_STATUS(H_ILLEGAL),
	MODE_STATUS(V_ILLEGAL),
	MODE_STATUS(BAD_WIDTH),
	MODE_STATUS(NOMODE),
	MODE_STATUS(NO_INTERLACE),
	MODE_STATUS(NO_DBLESCAN),
	MODE_STATUS(NO_VSCAN),
	MODE_STATUS(MEM),
	MODE_STATUS(VIRTUAL_X),
	MODE_STATUS(VIRTUAL_Y),
	MODE_STATUS(MEM_VIRT),
	MODE_STATUS(NOCLOCK),
	MODE_STATUS(CLOCK_HIGH),
	MODE_STATUS(CLOCK_LOW),
	MODE_STATUS(CLOCK_RANGE),
	MODE_STATUS(BAD_HVALUE),
	MODE_STATUS(BAD_VVALUE),
	MODE_STATUS(BAD_VSCAN),
	MODE_STATUS(HSYNC_NARROW),
	MODE_STATUS(HSYNC_WIDE),
	MODE_STATUS(HBLANK_NARROW),
	MODE_STATUS(HBLANK_WIDE),
	MODE_STATUS(VSYNC_NARROW),
	MODE_STATUS(VSYNC_WIDE),
	MODE_STATUS(VBLANK_NARROW),
	MODE_STATUS(VBLANK_WIDE),
	MODE_STATUS(PANEL),
	MODE_STATUS(INTERLACE_WIDTH),
	MODE_STATUS(ONE_WIDTH),
	MODE_STATUS(ONE_HEIGHT),
	MODE_STATUS(ONE_SIZE),
	MODE_STATUS(NO_REDUCED),
	MODE_STATUS(NO_STEREO),
	MODE_STATUS(NO_420),
	MODE_STATUS(STALE),
	MODE_STATUS(BAD),
	MODE_STATUS(ERROR),
};

#undef MODE_STATUS

const char *drm_get_mode_status_name(enum drm_mode_status status)
{
	int index = (int)status + 3;

	if (index < 0 || index >= (int)ARRAY_SIZE(drm_mode_status_names) ||
	    !drm_mode_status_names[index])
		return "";

	return drm_mode_status_names[index];
}

/* ---- mode lists ------------------------------------------------------------------ */

/**
 * drm_mode_prune_invalid - remove the modes that failed validation
 *
 * Every mode whose status is not MODE_OK leaves the array; the rest keep
 * their order.  A refused user-defined mode is worth a warning; the others
 * are logged at debug level when `verbose'.
 */
int drm_mode_prune_invalid(struct drm_display_mode *modes, int n,
			   bool verbose)
{
	int i, out = 0;

	for (i = 0; i < n; i++) {
		struct drm_display_mode *mode = &modes[i];

		if (mode->status != MODE_OK) {
			if (mode->type & DRM_MODE_TYPE_USERDEF) {
				modes_warn("User-defined mode not supported: "
					   DRM_MODE_FMT "\n", DRM_MODE_ARG(mode));
			}
			if (verbose) {
				modes_dbg("Rejected mode: " DRM_MODE_FMT " (%s)\n",
					  DRM_MODE_ARG(mode),
					  drm_get_mode_status_name(mode->status));
			}
			continue;
		}
		if (out != i)
			drm_mode_copy(&modes[out], mode);
		out++;
	}
	return out;
}

/*
 * Negative if a is the better mode, zero if they are equivalent, positive
 * if b is: preferred first, then the larger, then the higher refresh rate,
 * then the higher clock.
 */
static int drm_mode_compare(const struct drm_display_mode *a,
			    const struct drm_display_mode *b)
{
	int diff;

	diff = ((b->type & DRM_MODE_TYPE_PREFERRED) != 0) -
		((a->type & DRM_MODE_TYPE_PREFERRED) != 0);
	if (diff)
		return diff;
	diff = b->hdisplay * b->vdisplay - a->hdisplay * a->vdisplay;
	if (diff)
		return diff;

	diff = drm_mode_vrefresh(b) - drm_mode_vrefresh(a);
	if (diff)
		return diff;

	diff = b->clock - a->clock;
	return diff;
}

/**
 * drm_mode_sort - sort a mode array by favourability
 *
 * The best modes move to the front; equivalent modes keep their order (an
 * insertion sort: the arrays are a connector's list, at most a few hundred
 * entries, and are sorted once per probe).
 */
void drm_mode_sort(struct drm_display_mode *modes, int n)
{
	struct drm_display_mode tmp;
	int i, j;

	for (i = 1; i < n; i++) {
		if (drm_mode_compare(&modes[i - 1], &modes[i]) <= 0)
			continue;
		drm_mode_copy(&tmp, &modes[i]);
		j = i;
		while (j > 0 && drm_mode_compare(&modes[j - 1], &tmp) > 0) {
			drm_mode_copy(&modes[j], &modes[j - 1]);
			j--;
		}
		drm_mode_copy(&modes[j], &tmp);
	}
}

/**
 * drm_mode_list_update - merge freshly probed modes into a mode list
 * @modes: the list, n entries, room for cap
 * @probed: the probed modes, nprobed entries
 *
 * A probed mode equal to one already in the list does not add an entry.
 * If the old one is stale (left over from an earlier probe) it is replaced
 * outright; otherwise the type bits of the two are merged, and of two
 * equal modes the one marked preferred supplies the timings (in case the
 * match is not 100%), the one already there when both or neither are.
 * A new mode goes to the end while there is room.  The modes are not
 * validated or pruned here.  Returns the new count.
 */
int drm_mode_list_update(struct drm_display_mode *modes, int n, int cap,
			 const struct drm_display_mode *probed, int nprobed)
{
	int p, i;

	for (p = 0; p < nprobed; p++) {
		const struct drm_display_mode *pmode = &probed[p];
		bool found_it = false;

		/* go through current modes checking for the new probed mode */
		for (i = 0; i < n; i++) {
			struct drm_display_mode *mode = &modes[i];

			if (!drm_display_mode_equal(pmode, mode))
				continue;

			found_it = true;

			if (mode->status == MODE_STALE) {
				drm_mode_copy(mode, pmode);
			} else if ((mode->type & DRM_MODE_TYPE_PREFERRED) == 0 &&
				   (pmode->type & DRM_MODE_TYPE_PREFERRED) != 0) {
				uint8_t type = (uint8_t)(mode->type | pmode->type);

				drm_mode_copy(mode, pmode);
				mode->type = type;
			} else {
				mode->type |= pmode->type;
			}
			break;
		}

		if (!found_it && n < cap)
			drm_mode_copy(&modes[n++], pmode);
	}
	return n;
}

/**
 * drm_mode_set_preferred - mark the modes of a resolution preferred
 */
void drm_mode_set_preferred(struct drm_display_mode *modes, int n,
			    int hpref, int vpref)
{
	int i;

	for (i = 0; i < n; i++) {
		if (modes[i].hdisplay == hpref &&
		    modes[i].vdisplay == vpref)
			modes[i].type |= DRM_MODE_TYPE_PREFERRED;
	}
}

/* ---- client form ------------------------------------------------------------------ */

/**
 * drm_mode_convert_to_umode - a core mode as the client sees it
 */
void drm_mode_convert_to_umode(struct drm_mode_modeinfo *out,
			       const struct drm_display_mode *in)
{
	drm_mode_to_umode(out, in);
}

/**
 * drm_mode_convert_umode - a client's mode as the core's, validated
 *
 * Returns 0 with the crtc values filled in, -ERANGE for a clock no int
 * holds, or -EINVAL for an unknown picture aspect ratio or a mode that
 * fails drm_mode_validate_driver() (out->status then says why).
 */
int drm_mode_convert_umode(struct drm_device *dev,
			   struct drm_display_mode *out,
			   const struct drm_mode_modeinfo *in)
{
	int ret = drm_mode_from_umode(out, in);

	if (ret)
		return ret;

	out->status = drm_mode_validate_driver(dev, out);
	if (out->status != MODE_OK)
		return -EINVAL;

	drm_mode_set_crtcinfo(out, CRTC_INTERLACE_HALVE_V);

	return 0;
}

/* ---- YCbCr 4:2:0 ------------------------------------------------------------------- */

static bool vic_bit(const unsigned long *map, uint8_t vic)
{
	const unsigned int bits = 8 * sizeof(unsigned long);

	return (map[vic / bits] >> (vic % bits)) & 1;
}

/**
 * drm_mode_is_420_only - can the sink take the mode only as YCbCr 4:2:0?
 */
bool drm_mode_is_420_only(const struct drm_display_info *display,
			  const struct drm_display_mode *mode)
{
	uint8_t vic = drm_match_cea_mode(mode);

	return vic_bit(display->hdmi.y420_vdb_modes, vic);
}

/**
 * drm_mode_is_420_also - can the sink take the mode as YCbCr 4:2:0 too
 * (besides RGB / YCbCr 4:4:4 / 4:2:2)?
 */
bool drm_mode_is_420_also(const struct drm_display_info *display,
			  const struct drm_display_mode *mode)
{
	uint8_t vic = drm_match_cea_mode(mode);

	return vic_bit(display->hdmi.y420_cmdb_modes, vic);
}

/**
 * drm_mode_is_420 - can the sink take the mode as YCbCr 4:2:0?
 */
bool drm_mode_is_420(const struct drm_display_info *display,
		     const struct drm_display_mode *mode)
{
	return drm_mode_is_420_only(display, mode) ||
		drm_mode_is_420_also(display, mode);
}

/* ---- the standard timing tables -------------------------------------------- */

/*
 * VESA Display Monitor Timings, in DMT ID order (the ID is in the comment
 * above each entry).
 */
static const struct drm_display_mode drm_dmt_modes[] = {
	/* 0x01 - 640x350@85Hz */
	{ DRM_MODE("640x350", DRM_MODE_TYPE_DRIVER, 31500, 640, 672,
		   736, 832, 0, 350, 382, 385, 445, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x02 - 640x400@85Hz */
	{ DRM_MODE("640x400", DRM_MODE_TYPE_DRIVER, 31500, 640, 672,
		   736, 832, 0, 400, 401, 404, 445, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x03 - 720x400@85Hz */
	{ DRM_MODE("720x400", DRM_MODE_TYPE_DRIVER, 35500, 720, 756,
		   828, 936, 0, 400, 401, 404, 446, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x04 - 640x480@60Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656,
		   752, 800, 0, 480, 490, 492, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x05 - 640x480@72Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 664,
		   704, 832, 0, 480, 489, 492, 520, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x06 - 640x480@75Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 31500, 640, 656,
		   720, 840, 0, 480, 481, 484, 500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x07 - 640x480@85Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 36000, 640, 696,
		   752, 832, 0, 480, 481, 484, 509, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x08 - 800x600@56Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 36000, 800, 824,
		   896, 1024, 0, 600, 601, 603, 625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x09 - 800x600@60Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 40000, 800, 840,
		   968, 1056, 0, 600, 601, 605, 628, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x0a - 800x600@72Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 50000, 800, 856,
		   976, 1040, 0, 600, 637, 643, 666, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x0b - 800x600@75Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 49500, 800, 816,
		   896, 1056, 0, 600, 601, 604, 625, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x0c - 800x600@85Hz */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 56250, 800, 832,
		   896, 1048, 0, 600, 601, 604, 631, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x0d - 800x600@120Hz RB */
	{ DRM_MODE("800x600", DRM_MODE_TYPE_DRIVER, 73250, 800, 848,
		   880, 960, 0, 600, 603, 607, 636, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x0e - 848x480@60Hz */
	{ DRM_MODE("848x480", DRM_MODE_TYPE_DRIVER, 33750, 848, 864,
		   976, 1088, 0, 480, 486, 494, 517, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x0f - 1024x768@43Hz, interlace */
	{ DRM_MODE("1024x768i", DRM_MODE_TYPE_DRIVER, 44900, 1024, 1032,
		   1208, 1264, 0, 768, 768, 776, 817, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
		   DRM_MODE_FLAG_INTERLACE) },
	/* 0x10 - 1024x768@60Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 65000, 1024, 1048,
		   1184, 1344, 0, 768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x11 - 1024x768@70Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 75000, 1024, 1048,
		   1184, 1328, 0, 768, 771, 777, 806, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x12 - 1024x768@75Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 78750, 1024, 1040,
		   1136, 1312, 0, 768, 769, 772, 800, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x13 - 1024x768@85Hz */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 94500, 1024, 1072,
		   1168, 1376, 0, 768, 769, 772, 808, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x14 - 1024x768@120Hz RB */
	{ DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER, 115500, 1024, 1072,
		   1104, 1184, 0, 768, 771, 775, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x15 - 1152x864@75Hz */
	{ DRM_MODE("1152x864", DRM_MODE_TYPE_DRIVER, 108000, 1152, 1216,
		   1344, 1600, 0, 864, 865, 868, 900, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x55 - 1280x720@60Hz */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x16 - 1280x768@60Hz RB */
	{ DRM_MODE("1280x768", DRM_MODE_TYPE_DRIVER, 68250, 1280, 1328,
		   1360, 1440, 0, 768, 771, 778, 790, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x17 - 1280x768@60Hz */
	{ DRM_MODE("1280x768", DRM_MODE_TYPE_DRIVER, 79500, 1280, 1344,
		   1472, 1664, 0, 768, 771, 778, 798, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x18 - 1280x768@75Hz */
	{ DRM_MODE("1280x768", DRM_MODE_TYPE_DRIVER, 102250, 1280, 1360,
		   1488, 1696, 0, 768, 771, 778, 805, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x19 - 1280x768@85Hz */
	{ DRM_MODE("1280x768", DRM_MODE_TYPE_DRIVER, 117500, 1280, 1360,
		   1496, 1712, 0, 768, 771, 778, 809, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x1a - 1280x768@120Hz RB */
	{ DRM_MODE("1280x768", DRM_MODE_TYPE_DRIVER, 140250, 1280, 1328,
		   1360, 1440, 0, 768, 771, 778, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x1b - 1280x800@60Hz RB */
	{ DRM_MODE("1280x800", DRM_MODE_TYPE_DRIVER, 71000, 1280, 1328,
		   1360, 1440, 0, 800, 803, 809, 823, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x1c - 1280x800@60Hz */
	{ DRM_MODE("1280x800", DRM_MODE_TYPE_DRIVER, 83500, 1280, 1352,
		   1480, 1680, 0, 800, 803, 809, 831, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x1d - 1280x800@75Hz */
	{ DRM_MODE("1280x800", DRM_MODE_TYPE_DRIVER, 106500, 1280, 1360,
		   1488, 1696, 0, 800, 803, 809, 838, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x1e - 1280x800@85Hz */
	{ DRM_MODE("1280x800", DRM_MODE_TYPE_DRIVER, 122500, 1280, 1360,
		   1496, 1712, 0, 800, 803, 809, 843, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x1f - 1280x800@120Hz RB */
	{ DRM_MODE("1280x800", DRM_MODE_TYPE_DRIVER, 146250, 1280, 1328,
		   1360, 1440, 0, 800, 803, 809, 847, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x20 - 1280x960@60Hz */
	{ DRM_MODE("1280x960", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1376,
		   1488, 1800, 0, 960, 961, 964, 1000, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x21 - 1280x960@85Hz */
	{ DRM_MODE("1280x960", DRM_MODE_TYPE_DRIVER, 148500, 1280, 1344,
		   1504, 1728, 0, 960, 961, 964, 1011, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x22 - 1280x960@120Hz RB */
	{ DRM_MODE("1280x960", DRM_MODE_TYPE_DRIVER, 175500, 1280, 1328,
		   1360, 1440, 0, 960, 963, 967, 1017, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x23 - 1280x1024@60Hz */
	{ DRM_MODE("1280x1024", DRM_MODE_TYPE_DRIVER, 108000, 1280, 1328,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x24 - 1280x1024@75Hz */
	{ DRM_MODE("1280x1024", DRM_MODE_TYPE_DRIVER, 135000, 1280, 1296,
		   1440, 1688, 0, 1024, 1025, 1028, 1066, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x25 - 1280x1024@85Hz */
	{ DRM_MODE("1280x1024", DRM_MODE_TYPE_DRIVER, 157500, 1280, 1344,
		   1504, 1728, 0, 1024, 1025, 1028, 1072, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x26 - 1280x1024@120Hz RB */
	{ DRM_MODE("1280x1024", DRM_MODE_TYPE_DRIVER, 187250, 1280, 1328,
		   1360, 1440, 0, 1024, 1027, 1034, 1084, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x27 - 1360x768@60Hz */
	{ DRM_MODE("1360x768", DRM_MODE_TYPE_DRIVER, 85500, 1360, 1424,
		   1536, 1792, 0, 768, 771, 777, 795, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x28 - 1360x768@120Hz RB */
	{ DRM_MODE("1360x768", DRM_MODE_TYPE_DRIVER, 148250, 1360, 1408,
		   1440, 1520, 0, 768, 771, 776, 813, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x51 - 1366x768@60Hz */
	{ DRM_MODE("1366x768", DRM_MODE_TYPE_DRIVER, 85500, 1366, 1436,
		   1579, 1792, 0, 768, 771, 774, 798, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x56 - 1366x768@60Hz */
	{ DRM_MODE("1366x768", DRM_MODE_TYPE_DRIVER, 72000, 1366, 1380,
		   1436, 1500, 0, 768, 769, 772, 800, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x29 - 1400x1050@60Hz RB */
	{ DRM_MODE("1400x1050", DRM_MODE_TYPE_DRIVER, 101000, 1400, 1448,
		   1480, 1560, 0, 1050, 1053, 1057, 1080, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x2a - 1400x1050@60Hz */
	{ DRM_MODE("1400x1050", DRM_MODE_TYPE_DRIVER, 121750, 1400, 1488,
		   1632, 1864, 0, 1050, 1053, 1057, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x2b - 1400x1050@75Hz */
	{ DRM_MODE("1400x1050", DRM_MODE_TYPE_DRIVER, 156000, 1400, 1504,
		   1648, 1896, 0, 1050, 1053, 1057, 1099, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x2c - 1400x1050@85Hz */
	{ DRM_MODE("1400x1050", DRM_MODE_TYPE_DRIVER, 179500, 1400, 1504,
		   1656, 1912, 0, 1050, 1053, 1057, 1105, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x2d - 1400x1050@120Hz RB */
	{ DRM_MODE("1400x1050", DRM_MODE_TYPE_DRIVER, 208000, 1400, 1448,
		   1480, 1560, 0, 1050, 1053, 1057, 1112, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x2e - 1440x900@60Hz RB */
	{ DRM_MODE("1440x900", DRM_MODE_TYPE_DRIVER, 88750, 1440, 1488,
		   1520, 1600, 0, 900, 903, 909, 926, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x2f - 1440x900@60Hz */
	{ DRM_MODE("1440x900", DRM_MODE_TYPE_DRIVER, 106500, 1440, 1520,
		   1672, 1904, 0, 900, 903, 909, 934, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x30 - 1440x900@75Hz */
	{ DRM_MODE("1440x900", DRM_MODE_TYPE_DRIVER, 136750, 1440, 1536,
		   1688, 1936, 0, 900, 903, 909, 942, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x31 - 1440x900@85Hz */
	{ DRM_MODE("1440x900", DRM_MODE_TYPE_DRIVER, 157000, 1440, 1544,
		   1696, 1952, 0, 900, 903, 909, 948, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x32 - 1440x900@120Hz RB */
	{ DRM_MODE("1440x900", DRM_MODE_TYPE_DRIVER, 182750, 1440, 1488,
		   1520, 1600, 0, 900, 903, 909, 953, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x53 - 1600x900@60Hz */
	{ DRM_MODE("1600x900", DRM_MODE_TYPE_DRIVER, 108000, 1600, 1624,
		   1704, 1800, 0, 900, 901, 904, 1000, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x33 - 1600x1200@60Hz */
	{ DRM_MODE("1600x1200", DRM_MODE_TYPE_DRIVER, 162000, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x34 - 1600x1200@65Hz */
	{ DRM_MODE("1600x1200", DRM_MODE_TYPE_DRIVER, 175500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x35 - 1600x1200@70Hz */
	{ DRM_MODE("1600x1200", DRM_MODE_TYPE_DRIVER, 189000, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x36 - 1600x1200@75Hz */
	{ DRM_MODE("1600x1200", DRM_MODE_TYPE_DRIVER, 202500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x37 - 1600x1200@85Hz */
	{ DRM_MODE("1600x1200", DRM_MODE_TYPE_DRIVER, 229500, 1600, 1664,
		   1856, 2160, 0, 1200, 1201, 1204, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x38 - 1600x1200@120Hz RB */
	{ DRM_MODE("1600x1200", DRM_MODE_TYPE_DRIVER, 268250, 1600, 1648,
		   1680, 1760, 0, 1200, 1203, 1207, 1271, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x39 - 1680x1050@60Hz RB */
	{ DRM_MODE("1680x1050", DRM_MODE_TYPE_DRIVER, 119000, 1680, 1728,
		   1760, 1840, 0, 1050, 1053, 1059, 1080, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x3a - 1680x1050@60Hz */
	{ DRM_MODE("1680x1050", DRM_MODE_TYPE_DRIVER, 146250, 1680, 1784,
		   1960, 2240, 0, 1050, 1053, 1059, 1089, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x3b - 1680x1050@75Hz */
	{ DRM_MODE("1680x1050", DRM_MODE_TYPE_DRIVER, 187000, 1680, 1800,
		   1976, 2272, 0, 1050, 1053, 1059, 1099, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x3c - 1680x1050@85Hz */
	{ DRM_MODE("1680x1050", DRM_MODE_TYPE_DRIVER, 214750, 1680, 1808,
		   1984, 2288, 0, 1050, 1053, 1059, 1105, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x3d - 1680x1050@120Hz RB */
	{ DRM_MODE("1680x1050", DRM_MODE_TYPE_DRIVER, 245500, 1680, 1728,
		   1760, 1840, 0, 1050, 1053, 1059, 1112, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x3e - 1792x1344@60Hz */
	{ DRM_MODE("1792x1344", DRM_MODE_TYPE_DRIVER, 204750, 1792, 1920,
		   2120, 2448, 0, 1344, 1345, 1348, 1394, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x3f - 1792x1344@75Hz */
	{ DRM_MODE("1792x1344", DRM_MODE_TYPE_DRIVER, 261000, 1792, 1888,
		   2104, 2456, 0, 1344, 1345, 1348, 1417, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x40 - 1792x1344@120Hz RB */
	{ DRM_MODE("1792x1344", DRM_MODE_TYPE_DRIVER, 333250, 1792, 1840,
		   1872, 1952, 0, 1344, 1347, 1351, 1423, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x41 - 1856x1392@60Hz */
	{ DRM_MODE("1856x1392", DRM_MODE_TYPE_DRIVER, 218250, 1856, 1952,
		   2176, 2528, 0, 1392, 1393, 1396, 1439, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x42 - 1856x1392@75Hz */
	{ DRM_MODE("1856x1392", DRM_MODE_TYPE_DRIVER, 288000, 1856, 1984,
		   2208, 2560, 0, 1392, 1393, 1396, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x43 - 1856x1392@120Hz RB */
	{ DRM_MODE("1856x1392", DRM_MODE_TYPE_DRIVER, 356500, 1856, 1904,
		   1936, 2016, 0, 1392, 1395, 1399, 1474, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x52 - 1920x1080@60Hz */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x44 - 1920x1200@60Hz RB */
	{ DRM_MODE("1920x1200", DRM_MODE_TYPE_DRIVER, 154000, 1920, 1968,
		   2000, 2080, 0, 1200, 1203, 1209, 1235, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x45 - 1920x1200@60Hz */
	{ DRM_MODE("1920x1200", DRM_MODE_TYPE_DRIVER, 193250, 1920, 2056,
		   2256, 2592, 0, 1200, 1203, 1209, 1245, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x46 - 1920x1200@75Hz */
	{ DRM_MODE("1920x1200", DRM_MODE_TYPE_DRIVER, 245250, 1920, 2056,
		   2264, 2608, 0, 1200, 1203, 1209, 1255, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x47 - 1920x1200@85Hz */
	{ DRM_MODE("1920x1200", DRM_MODE_TYPE_DRIVER, 281250, 1920, 2064,
		   2272, 2624, 0, 1200, 1203, 1209, 1262, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x48 - 1920x1200@120Hz RB */
	{ DRM_MODE("1920x1200", DRM_MODE_TYPE_DRIVER, 317000, 1920, 1968,
		   2000, 2080, 0, 1200, 1203, 1209, 1271, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x49 - 1920x1440@60Hz */
	{ DRM_MODE("1920x1440", DRM_MODE_TYPE_DRIVER, 234000, 1920, 2048,
		   2256, 2600, 0, 1440, 1441, 1444, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x4a - 1920x1440@75Hz */
	{ DRM_MODE("1920x1440", DRM_MODE_TYPE_DRIVER, 297000, 1920, 2064,
		   2288, 2640, 0, 1440, 1441, 1444, 1500, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x4b - 1920x1440@120Hz RB */
	{ DRM_MODE("1920x1440", DRM_MODE_TYPE_DRIVER, 380500, 1920, 1968,
		   2000, 2080, 0, 1440, 1443, 1447, 1525, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x54 - 2048x1152@60Hz */
	{ DRM_MODE("2048x1152", DRM_MODE_TYPE_DRIVER, 162000, 2048, 2074,
		   2154, 2250, 0, 1152, 1153, 1156, 1200, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x4c - 2560x1600@60Hz RB */
	{ DRM_MODE("2560x1600", DRM_MODE_TYPE_DRIVER, 268500, 2560, 2608,
		   2640, 2720, 0, 1600, 1603, 1609, 1646, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x4d - 2560x1600@60Hz */
	{ DRM_MODE("2560x1600", DRM_MODE_TYPE_DRIVER, 348500, 2560, 2752,
		   3032, 3504, 0, 1600, 1603, 1609, 1658, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x4e - 2560x1600@75Hz */
	{ DRM_MODE("2560x1600", DRM_MODE_TYPE_DRIVER, 443250, 2560, 2768,
		   3048, 3536, 0, 1600, 1603, 1609, 1672, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x4f - 2560x1600@85Hz */
	{ DRM_MODE("2560x1600", DRM_MODE_TYPE_DRIVER, 505250, 2560, 2768,
		   3048, 3536, 0, 1600, 1603, 1609, 1682, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x50 - 2560x1600@120Hz RB */
	{ DRM_MODE("2560x1600", DRM_MODE_TYPE_DRIVER, 552750, 2560, 2608,
		   2640, 2720, 0, 1600, 1603, 1609, 1694, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x57 - 4096x2160@60Hz RB */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 556744, 4096, 4104,
		   4136, 4176, 0, 2160, 2208, 2216, 2222, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x58 - 4096x2160@59.94Hz RB */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 556188, 4096, 4104,
		   4136, 4176, 0, 2160, 2208, 2216, 2222, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC) },
};

/*
 * From CEA/CTA-861 spec.
 *
 * Do not access directly, instead always use drm_cea_mode_for_vic().
 */
static const struct drm_display_mode edid_cea_modes_1[] = {
	/* 1 - 640x480@60Hz 4:3 */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656,
		   752, 800, 0, 480, 490, 492, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 2 - 720x480@60Hz 4:3 */
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 27000, 720, 736,
		   798, 858, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 3 - 720x480@60Hz 16:9 */
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 27000, 720, 736,
		   798, 858, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 4 - 1280x720@60Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 5 - 1920x1080i@60Hz 16:9 */
	{ DRM_MODE("1920x1080i", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1094, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 6 - 720(1440)x480i@60Hz 4:3 */
	{ DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 13500, 720, 739,
		   801, 858, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 7 - 720(1440)x480i@60Hz 16:9 */
	{ DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 13500, 720, 739,
		   801, 858, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 8 - 720(1440)x240@60Hz 4:3 */
	{ DRM_MODE("720x240", DRM_MODE_TYPE_DRIVER, 13500, 720, 739,
		   801, 858, 0, 240, 244, 247, 262, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 9 - 720(1440)x240@60Hz 16:9 */
	{ DRM_MODE("720x240", DRM_MODE_TYPE_DRIVER, 13500, 720, 739,
		   801, 858, 0, 240, 244, 247, 262, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 10 - 2880x480i@60Hz 4:3 */
	{ DRM_MODE("2880x480i", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2956,
		   3204, 3432, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 11 - 2880x480i@60Hz 16:9 */
	{ DRM_MODE("2880x480i", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2956,
		   3204, 3432, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 12 - 2880x240@60Hz 4:3 */
	{ DRM_MODE("2880x240", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2956,
		   3204, 3432, 0, 240, 244, 247, 262, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 13 - 2880x240@60Hz 16:9 */
	{ DRM_MODE("2880x240", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2956,
		   3204, 3432, 0, 240, 244, 247, 262, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 14 - 1440x480@60Hz 4:3 */
	{ DRM_MODE("1440x480", DRM_MODE_TYPE_DRIVER, 54000, 1440, 1472,
		   1596, 1716, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 15 - 1440x480@60Hz 16:9 */
	{ DRM_MODE("1440x480", DRM_MODE_TYPE_DRIVER, 54000, 1440, 1472,
		   1596, 1716, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 16 - 1920x1080@60Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 17 - 720x576@50Hz 4:3 */
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 27000, 720, 732,
		   796, 864, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 18 - 720x576@50Hz 16:9 */
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 27000, 720, 732,
		   796, 864, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 19 - 1280x720@50Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1720,
		   1760, 1980, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 20 - 1920x1080i@50Hz 16:9 */
	{ DRM_MODE("1920x1080i", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1094, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 21 - 720(1440)x576i@50Hz 4:3 */
	{ DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 13500, 720, 732,
		   795, 864, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 22 - 720(1440)x576i@50Hz 16:9 */
	{ DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 13500, 720, 732,
		   795, 864, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 23 - 720(1440)x288@50Hz 4:3 */
	{ DRM_MODE("720x288", DRM_MODE_TYPE_DRIVER, 13500, 720, 732,
		   795, 864, 0, 288, 290, 293, 312, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 24 - 720(1440)x288@50Hz 16:9 */
	{ DRM_MODE("720x288", DRM_MODE_TYPE_DRIVER, 13500, 720, 732,
		   795, 864, 0, 288, 290, 293, 312, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 25 - 2880x576i@50Hz 4:3 */
	{ DRM_MODE("2880x576i", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2928,
		   3180, 3456, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 26 - 2880x576i@50Hz 16:9 */
	{ DRM_MODE("2880x576i", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2928,
		   3180, 3456, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 27 - 2880x288@50Hz 4:3 */
	{ DRM_MODE("2880x288", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2928,
		   3180, 3456, 0, 288, 290, 293, 312, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 28 - 2880x288@50Hz 16:9 */
	{ DRM_MODE("2880x288", DRM_MODE_TYPE_DRIVER, 54000, 2880, 2928,
		   3180, 3456, 0, 288, 290, 293, 312, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 29 - 1440x576@50Hz 4:3 */
	{ DRM_MODE("1440x576", DRM_MODE_TYPE_DRIVER, 54000, 1440, 1464,
		   1592, 1728, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 30 - 1440x576@50Hz 16:9 */
	{ DRM_MODE("1440x576", DRM_MODE_TYPE_DRIVER, 54000, 1440, 1464,
		   1592, 1728, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 31 - 1920x1080@50Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 32 - 1920x1080@24Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2558,
		   2602, 2750, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 33 - 1920x1080@25Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 34 - 1920x1080@30Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 35 - 2880x480@60Hz 4:3 */
	{ DRM_MODE("2880x480", DRM_MODE_TYPE_DRIVER, 108000, 2880, 2944,
		   3192, 3432, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 36 - 2880x480@60Hz 16:9 */
	{ DRM_MODE("2880x480", DRM_MODE_TYPE_DRIVER, 108000, 2880, 2944,
		   3192, 3432, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 37 - 2880x576@50Hz 4:3 */
	{ DRM_MODE("2880x576", DRM_MODE_TYPE_DRIVER, 108000, 2880, 2928,
		   3184, 3456, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 38 - 2880x576@50Hz 16:9 */
	{ DRM_MODE("2880x576", DRM_MODE_TYPE_DRIVER, 108000, 2880, 2928,
		   3184, 3456, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 39 - 1920x1080i@50Hz 16:9 */
	{ DRM_MODE("1920x1080i", DRM_MODE_TYPE_DRIVER, 72000, 1920, 1952,
		   2120, 2304, 0, 1080, 1126, 1136, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 40 - 1920x1080i@100Hz 16:9 */
	{ DRM_MODE("1920x1080i", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1094, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 41 - 1280x720@100Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 148500, 1280, 1720,
		   1760, 1980, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 42 - 720x576@100Hz 4:3 */
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 54000, 720, 732,
		   796, 864, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 43 - 720x576@100Hz 16:9 */
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 54000, 720, 732,
		   796, 864, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 44 - 720(1440)x576i@100Hz 4:3 */
	{ DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 27000, 720, 732,
		   795, 864, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 45 - 720(1440)x576i@100Hz 16:9 */
	{ DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 27000, 720, 732,
		   795, 864, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 46 - 1920x1080i@120Hz 16:9 */
	{ DRM_MODE("1920x1080i", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1094, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
		   DRM_MODE_FLAG_INTERLACE),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 47 - 1280x720@120Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 148500, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 48 - 720x480@120Hz 4:3 */
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 54000, 720, 736,
		   798, 858, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 49 - 720x480@120Hz 16:9 */
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 54000, 720, 736,
		   798, 858, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 50 - 720(1440)x480i@120Hz 4:3 */
	{ DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 27000, 720, 739,
		   801, 858, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 51 - 720(1440)x480i@120Hz 16:9 */
	{ DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 27000, 720, 739,
		   801, 858, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 52 - 720x576@200Hz 4:3 */
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 108000, 720, 732,
		   796, 864, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 53 - 720x576@200Hz 16:9 */
	{ DRM_MODE("720x576", DRM_MODE_TYPE_DRIVER, 108000, 720, 732,
		   796, 864, 0, 576, 581, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 54 - 720(1440)x576i@200Hz 4:3 */
	{ DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 54000, 720, 732,
		   795, 864, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 55 - 720(1440)x576i@200Hz 16:9 */
	{ DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 54000, 720, 732,
		   795, 864, 0, 576, 580, 586, 625, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 56 - 720x480@240Hz 4:3 */
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 108000, 720, 736,
		   798, 858, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 57 - 720x480@240Hz 16:9 */
	{ DRM_MODE("720x480", DRM_MODE_TYPE_DRIVER, 108000, 720, 736,
		   798, 858, 0, 480, 489, 495, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 58 - 720(1440)x480i@240Hz 4:3 */
	{ DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 54000, 720, 739,
		   801, 858, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_4_3, },
	/* 59 - 720(1440)x480i@240Hz 16:9 */
	{ DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 54000, 720, 739,
		   801, 858, 0, 480, 488, 494, 525, 0,
		   DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
		   DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 60 - 1280x720@24Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 59400, 1280, 3040,
		   3080, 3300, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 61 - 1280x720@25Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 3700,
		   3740, 3960, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 62 - 1280x720@30Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 3040,
		   3080, 3300, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 63 - 1920x1080@120Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 297000, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 64 - 1920x1080@100Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 297000, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 65 - 1280x720@24Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 59400, 1280, 3040,
		   3080, 3300, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 66 - 1280x720@25Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 3700,
		   3740, 3960, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 67 - 1280x720@30Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 3040,
		   3080, 3300, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 68 - 1280x720@50Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1720,
		   1760, 1980, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 69 - 1280x720@60Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 70 - 1280x720@100Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 148500, 1280, 1720,
		   1760, 1980, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 71 - 1280x720@120Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 148500, 1280, 1390,
		   1430, 1650, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 72 - 1920x1080@24Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2558,
		   2602, 2750, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 73 - 1920x1080@25Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 74 - 1920x1080@30Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 74250, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 75 - 1920x1080@50Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 76 - 1920x1080@60Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 77 - 1920x1080@100Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 297000, 1920, 2448,
		   2492, 2640, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 78 - 1920x1080@120Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 297000, 1920, 2008,
		   2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 79 - 1680x720@24Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 59400, 1680, 3040,
		   3080, 3300, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 80 - 1680x720@25Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 59400, 1680, 2908,
		   2948, 3168, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 81 - 1680x720@30Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 59400, 1680, 2380,
		   2420, 2640, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 82 - 1680x720@50Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 82500, 1680, 1940,
		   1980, 2200, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 83 - 1680x720@60Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 99000, 1680, 1940,
		   1980, 2200, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 84 - 1680x720@100Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 165000, 1680, 1740,
		   1780, 2000, 0, 720, 725, 730, 825, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 85 - 1680x720@120Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 198000, 1680, 1740,
		   1780, 2000, 0, 720, 725, 730, 825, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 86 - 2560x1080@24Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 99000, 2560, 3558,
		   3602, 3750, 0, 1080, 1084, 1089, 1100, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 87 - 2560x1080@25Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 90000, 2560, 3008,
		   3052, 3200, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 88 - 2560x1080@30Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 118800, 2560, 3328,
		   3372, 3520, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 89 - 2560x1080@50Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 185625, 2560, 3108,
		   3152, 3300, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 90 - 2560x1080@60Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 198000, 2560, 2808,
		   2852, 3000, 0, 1080, 1084, 1089, 1100, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 91 - 2560x1080@100Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 371250, 2560, 2778,
		   2822, 2970, 0, 1080, 1084, 1089, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 92 - 2560x1080@120Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 495000, 2560, 3108,
		   3152, 3300, 0, 1080, 1084, 1089, 1250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 93 - 3840x2160@24Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000, 3840, 5116,
		   5204, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 94 - 3840x2160@25Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000, 3840, 4896,
		   4984, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 95 - 3840x2160@30Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000, 3840, 4016,
		   4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 96 - 3840x2160@50Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 4896,
		   4984, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 97 - 3840x2160@60Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 4016,
		   4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 98 - 4096x2160@24Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 297000, 4096, 5116,
		   5204, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
	/* 99 - 4096x2160@25Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 297000, 4096, 5064,
		   5152, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
	/* 100 - 4096x2160@30Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 297000, 4096, 4184,
		   4272, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
	/* 101 - 4096x2160@50Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 594000, 4096, 5064,
		   5152, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
	/* 102 - 4096x2160@60Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 594000, 4096, 4184,
		   4272, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
	/* 103 - 3840x2160@24Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000, 3840, 5116,
		   5204, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 104 - 3840x2160@25Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000, 3840, 4896,
		   4984, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 105 - 3840x2160@30Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000, 3840, 4016,
		   4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 106 - 3840x2160@50Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 4896,
		   4984, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 107 - 3840x2160@60Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 4016,
		   4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 108 - 1280x720@48Hz 16:9 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 90000, 1280, 2240,
		   2280, 2500, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 109 - 1280x720@48Hz 64:27 */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 90000, 1280, 2240,
		   2280, 2500, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 110 - 1680x720@48Hz 64:27 */
	{ DRM_MODE("1680x720", DRM_MODE_TYPE_DRIVER, 99000, 1680, 2490,
		   2530, 2750, 0, 720, 725, 730, 750, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 111 - 1920x1080@48Hz 16:9 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2558,
		   2602, 2750, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 112 - 1920x1080@48Hz 64:27 */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2558,
		   2602, 2750, 0, 1080, 1084, 1089, 1125, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 113 - 2560x1080@48Hz 64:27 */
	{ DRM_MODE("2560x1080", DRM_MODE_TYPE_DRIVER, 198000, 2560, 3558,
		   3602, 3750, 0, 1080, 1084, 1089, 1100, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 114 - 3840x2160@48Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 5116,
		   5204, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 115 - 4096x2160@48Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 594000, 4096, 5116,
		   5204, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
	/* 116 - 3840x2160@48Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 5116,
		   5204, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 117 - 3840x2160@100Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 1188000, 3840, 4896,
		   4984, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 118 - 3840x2160@120Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 1188000, 3840, 4016,
		   4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 119 - 3840x2160@100Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 1188000, 3840, 4896,
		   4984, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 120 - 3840x2160@120Hz 64:27 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 1188000, 3840, 4016,
		   4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 121 - 5120x2160@24Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 396000, 5120, 7116,
		   7204, 7500, 0, 2160, 2168, 2178, 2200, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 122 - 5120x2160@25Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 396000, 5120, 6816,
		   6904, 7200, 0, 2160, 2168, 2178, 2200, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 123 - 5120x2160@30Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 396000, 5120, 5784,
		   5872, 6000, 0, 2160, 2168, 2178, 2200, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 124 - 5120x2160@48Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 742500, 5120, 5866,
		   5954, 6250, 0, 2160, 2168, 2178, 2475, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 125 - 5120x2160@50Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 742500, 5120, 6216,
		   6304, 6600, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 126 - 5120x2160@60Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 742500, 5120, 5284,
		   5372, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 127 - 5120x2160@100Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 1485000, 5120, 6216,
		   6304, 6600, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
};

/*
 * From CEA/CTA-861 spec.
 *
 * Do not access directly, instead always use drm_cea_mode_for_vic().
 */
static const struct drm_display_mode edid_cea_modes_193[] = {
	/* 193 - 5120x2160@120Hz 64:27 */
	{ DRM_MODE("5120x2160", DRM_MODE_TYPE_DRIVER, 1485000, 5120, 5284,
		   5372, 5500, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 194 - 7680x4320@24Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 1188000, 7680, 10232,
		   10408, 11000, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 195 - 7680x4320@25Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 1188000, 7680, 10032,
		   10208, 10800, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 196 - 7680x4320@30Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 1188000, 7680, 8232,
		   8408, 9000, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 197 - 7680x4320@48Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 2376000, 7680, 10232,
		   10408, 11000, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 198 - 7680x4320@50Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 2376000, 7680, 10032,
		   10208, 10800, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 199 - 7680x4320@60Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 2376000, 7680, 8232,
		   8408, 9000, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 200 - 7680x4320@100Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 4752000, 7680, 9792,
		   9968, 10560, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 201 - 7680x4320@120Hz 16:9 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 4752000, 7680, 8032,
		   8208, 8800, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 202 - 7680x4320@24Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 1188000, 7680, 10232,
		   10408, 11000, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 203 - 7680x4320@25Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 1188000, 7680, 10032,
		   10208, 10800, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 204 - 7680x4320@30Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 1188000, 7680, 8232,
		   8408, 9000, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 205 - 7680x4320@48Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 2376000, 7680, 10232,
		   10408, 11000, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 206 - 7680x4320@50Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 2376000, 7680, 10032,
		   10208, 10800, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 207 - 7680x4320@60Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 2376000, 7680, 8232,
		   8408, 9000, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 208 - 7680x4320@100Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 4752000, 7680, 9792,
		   9968, 10560, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 209 - 7680x4320@120Hz 64:27 */
	{ DRM_MODE("7680x4320", DRM_MODE_TYPE_DRIVER, 4752000, 7680, 8032,
		   8208, 8800, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 210 - 10240x4320@24Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 1485000, 10240, 11732,
		   11908, 12500, 0, 4320, 4336, 4356, 4950, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 211 - 10240x4320@25Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 1485000, 10240, 12732,
		   12908, 13500, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 212 - 10240x4320@30Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 1485000, 10240, 10528,
		   10704, 11000, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 213 - 10240x4320@48Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 2970000, 10240, 11732,
		   11908, 12500, 0, 4320, 4336, 4356, 4950, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 214 - 10240x4320@50Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 2970000, 10240, 12732,
		   12908, 13500, 0, 4320, 4336, 4356, 4400, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 215 - 10240x4320@60Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 2970000, 10240, 10528,
		   10704, 11000, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 216 - 10240x4320@100Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 5940000, 10240, 12432,
		   12608, 13200, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 217 - 10240x4320@120Hz 64:27 */
	{ DRM_MODE("10240x4320", DRM_MODE_TYPE_DRIVER, 5940000, 10240, 10528,
		   10704, 11000, 0, 4320, 4336, 4356, 4500, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_64_27, },
	/* 218 - 4096x2160@100Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 1188000, 4096, 4896,
		   4984, 5280, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
	/* 219 - 4096x2160@120Hz 256:135 */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 1188000, 4096, 4184,
		   4272, 4400, 0, 2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
};

/*
 * HDMI 1.4 4k modes. Index using the VIC.
 */
static const struct drm_display_mode edid_4k_modes[] = {
	/* 0 - dummy, VICs start at 1 */
	{ },
	/* 1 - 3840x2160@30Hz */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000,
		   3840, 4016, 4104, 4400, 0,
		   2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 2 - 3840x2160@25Hz */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000,
		   3840, 4896, 4984, 5280, 0,
		   2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 3 - 3840x2160@24Hz */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 297000,
		   3840, 5116, 5204, 5500, 0,
		   2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_16_9, },
	/* 4 - 4096x2160@24Hz (SMPTE) */
	{ DRM_MODE("4096x2160", DRM_MODE_TYPE_DRIVER, 297000,
		   4096, 5116, 5204, 5500, 0,
		   2160, 2168, 2178, 2250, 0,
		   DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	  .picture_aspect_ratio = DRM_MODE_PICTURE_ASPECT_256_135, },
};

/* ---- table access --------------------------------------------------------------- */

int drm_dmt_num_modes(void)
{
	return (int)ARRAY_SIZE(drm_dmt_modes);
}

const struct drm_display_mode *drm_dmt_mode(int i)
{
	if (i < 0 || i >= (int)ARRAY_SIZE(drm_dmt_modes))
		return NULL;
	return &drm_dmt_modes[i];
}

/**
 * drm_mode_is_rb - does the mode have CVT reduced-blanking (v1) timing?
 *
 * 160 pixels of horizontal blanking with a 32-pixel sync ending in its
 * middle, and a 3-line vertical front porch.
 */
bool drm_mode_is_rb(const struct drm_display_mode *mode)
{
	return (mode->htotal - mode->hdisplay == 160) &&
	       (mode->hsync_end - mode->hdisplay == 80) &&
	       (mode->hsync_end - mode->hsync_start == 32) &&
	       (mode->vsync_start - mode->vdisplay == 3);
}

/**
 * drm_mode_find_dmt - the DMT entry of a geometry and refresh rate
 * @rb: the reduced-blanking entry, or the other one
 *
 * Returns the table entry (copy it with drm_mode_init() to change it), or
 * NULL when DMT has no such mode.
 */
const struct drm_display_mode *drm_mode_find_dmt(int hsize, int vsize,
						 int fresh, bool rb)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(drm_dmt_modes); i++) {
		const struct drm_display_mode *ptr = &drm_dmt_modes[i];

		if (hsize != ptr->hdisplay)
			continue;
		if (vsize != ptr->vdisplay)
			continue;
		if (fresh != drm_mode_vrefresh(ptr))
			continue;
		if (rb != drm_mode_is_rb(ptr))
			continue;

		return ptr;
	}

	return NULL;
}

/* The two CEA tables cover 1-127 and 193-219 with no gaps. */
_Static_assert(1 + ARRAY_SIZE(edid_cea_modes_1) - 1 == 127,
	       "CEA video codes 1-127");
_Static_assert(193 + ARRAY_SIZE(edid_cea_modes_193) - 1 == 219,
	       "CEA video codes 193-219");

const struct drm_display_mode *drm_cea_mode_for_vic(uint8_t vic)
{
	if (vic >= 1 && vic < 1 + ARRAY_SIZE(edid_cea_modes_1))
		return &edid_cea_modes_1[vic - 1];
	if (vic >= 193 && vic < 193 + ARRAY_SIZE(edid_cea_modes_193))
		return &edid_cea_modes_193[vic - 193];
	return NULL;
}

uint8_t drm_cea_num_vics(void)
{
	return (uint8_t)(193 + ARRAY_SIZE(edid_cea_modes_193));
}

uint8_t drm_cea_next_vic(uint8_t vic)
{
	if (++vic == 1 + ARRAY_SIZE(edid_cea_modes_1))
		vic = 193;
	return vic;
}

/*
 * Calculate the alternate clock for the CEA mode
 * (60Hz vs. 59.94Hz etc.)
 */
unsigned int
drm_cea_mode_alternate_clock(const struct drm_display_mode *cea_mode)
{
	unsigned int clock = (unsigned int)cea_mode->clock;

	if (drm_mode_vrefresh(cea_mode) % 6 != 0)
		return clock;

	/*
	 * edid_cea_modes contains the 59.94Hz
	 * variant for 240 and 480 line modes,
	 * and the 60Hz variant otherwise.
	 */
	if (cea_mode->vdisplay == 240 || cea_mode->vdisplay == 480)
		clock = DIV_ROUND_CLOSEST(clock * 1001, 1000);
	else
		clock = DIV_ROUND_CLOSEST(clock * 1000, 1001);

	return clock;
}

bool
drm_cea_mode_alternate_timings(uint8_t vic, struct drm_display_mode *mode)
{
	/*
	 * For certain VICs the spec allows the vertical
	 * front porch to vary by one or two lines.
	 *
	 * cea_modes[] stores the variant with the shortest
	 * vertical front porch. We can adjust the mode to
	 * get the other variants by simply increasing the
	 * vertical front porch length.  (The table entries of
	 * 8, 9, 12, 13 have vtotal 262, those of 23, 24, 27, 28
	 * vtotal 312; the host test checks it.)
	 */
	if (((vic == 8 || vic == 9 ||
	      vic == 12 || vic == 13) && mode->vtotal < 263) ||
	    ((vic == 23 || vic == 24 ||
	      vic == 27 || vic == 28) && mode->vtotal < 314)) {
		mode->vsync_start++;
		mode->vsync_end++;
		mode->vtotal++;

		return true;
	}

	return false;
}

/**
 * drm_match_cea_mode_clock_tolerance - the CEA video code of a mode,
 * the clock within `clock_tolerance' kHz of either variant
 */
uint8_t drm_match_cea_mode_clock_tolerance(const struct drm_display_mode *to_match,
					   unsigned int clock_tolerance)
{
	unsigned int match_flags = DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_FLAGS;
	uint8_t vic;

	if (!to_match->clock)
		return 0;

	if (to_match->picture_aspect_ratio)
		match_flags |= DRM_MODE_MATCH_ASPECT_RATIO;

	for (vic = 1; vic < drm_cea_num_vics(); vic = drm_cea_next_vic(vic)) {
		struct drm_display_mode cea_mode;
		unsigned int clock1, clock2;

		drm_mode_init(&cea_mode, drm_cea_mode_for_vic(vic));

		/* Check both 60Hz and 59.94Hz */
		clock1 = (unsigned int)cea_mode.clock;
		clock2 = drm_cea_mode_alternate_clock(&cea_mode);

		if ((unsigned int)iabs(to_match->clock - (int)clock1) > clock_tolerance &&
		    (unsigned int)iabs(to_match->clock - (int)clock2) > clock_tolerance)
			continue;

		do {
			if (drm_mode_match(to_match, &cea_mode, match_flags))
				return vic;
		} while (drm_cea_mode_alternate_timings(vic, &cea_mode));
	}

	return 0;
}

/**
 * drm_match_cea_mode - look for a CEA mode matching given mode
 *
 * Return: The CEA Video ID (VIC) of the mode or 0 if it isn't a CEA-861
 * mode.
 */
uint8_t drm_match_cea_mode(const struct drm_display_mode *to_match)
{
	unsigned int match_flags = DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_FLAGS;
	uint8_t vic;

	if (!to_match->clock)
		return 0;

	if (to_match->picture_aspect_ratio)
		match_flags |= DRM_MODE_MATCH_ASPECT_RATIO;

	for (vic = 1; vic < drm_cea_num_vics(); vic = drm_cea_next_vic(vic)) {
		struct drm_display_mode cea_mode;
		unsigned int clock1, clock2;

		drm_mode_init(&cea_mode, drm_cea_mode_for_vic(vic));

		/* Check both 60Hz and 59.94Hz */
		clock1 = (unsigned int)cea_mode.clock;
		clock2 = drm_cea_mode_alternate_clock(&cea_mode);

		if (KHZ2PICOS(to_match->clock) != KHZ2PICOS(clock1) &&
		    KHZ2PICOS(to_match->clock) != KHZ2PICOS(clock2))
			continue;

		do {
			if (drm_mode_match(to_match, &cea_mode, match_flags))
				return vic;
		} while (drm_cea_mode_alternate_timings(vic, &cea_mode));
	}

	return 0;
}

bool drm_valid_cea_vic(uint8_t vic)
{
	return drm_cea_mode_for_vic(vic) != NULL;
}

int drm_get_cea_aspect_ratio(uint8_t video_code)
{
	const struct drm_display_mode *mode = drm_cea_mode_for_vic(video_code);

	if (mode)
		return mode->picture_aspect_ratio;

	return DRM_MODE_PICTURE_ASPECT_NONE;
}

const struct drm_display_mode *drm_hdmi_mode_for_vic(uint8_t vic)
{
	if (!drm_valid_hdmi_vic(vic))
		return NULL;
	return &edid_4k_modes[vic];
}

int drm_hdmi_num_vics(void)
{
	return (int)ARRAY_SIZE(edid_4k_modes);
}

int drm_get_hdmi_aspect_ratio(uint8_t video_code)
{
	if (video_code >= ARRAY_SIZE(edid_4k_modes))
		return DRM_MODE_PICTURE_ASPECT_NONE;
	return edid_4k_modes[video_code].picture_aspect_ratio;
}

/*
 * Calculate the alternate clock for HDMI modes (those from the HDMI vendor
 * specific block).
 */
static unsigned int
hdmi_mode_alternate_clock(const struct drm_display_mode *hdmi_mode)
{
	return drm_cea_mode_alternate_clock(hdmi_mode);
}

uint8_t drm_match_hdmi_mode_clock_tolerance(const struct drm_display_mode *to_match,
					    unsigned int clock_tolerance)
{
	unsigned int match_flags = DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_FLAGS;
	uint8_t vic;

	if (!to_match->clock)
		return 0;

	if (to_match->picture_aspect_ratio)
		match_flags |= DRM_MODE_MATCH_ASPECT_RATIO;

	for (vic = 1; vic < ARRAY_SIZE(edid_4k_modes); vic++) {
		const struct drm_display_mode *hdmi_mode = &edid_4k_modes[vic];
		unsigned int clock1, clock2;

		/* Make sure to also match alternate clocks */
		clock1 = (unsigned int)hdmi_mode->clock;
		clock2 = hdmi_mode_alternate_clock(hdmi_mode);

		if ((unsigned int)iabs(to_match->clock - (int)clock1) > clock_tolerance &&
		    (unsigned int)iabs(to_match->clock - (int)clock2) > clock_tolerance)
			continue;

		if (drm_mode_match(to_match, hdmi_mode, match_flags))
			return vic;
	}

	return 0;
}

/*
 * drm_match_hdmi_mode - look for a HDMI mode matching given mode
 *
 * An HDMI mode is one defined in the HDMI vendor specific block.
 *
 * Returns the HDMI Video ID (VIC) of the mode or 0 if it isn't one.
 */
uint8_t drm_match_hdmi_mode(const struct drm_display_mode *to_match)
{
	unsigned int match_flags = DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_FLAGS;
	uint8_t vic;

	if (!to_match->clock)
		return 0;

	if (to_match->picture_aspect_ratio)
		match_flags |= DRM_MODE_MATCH_ASPECT_RATIO;

	for (vic = 1; vic < ARRAY_SIZE(edid_4k_modes); vic++) {
		const struct drm_display_mode *hdmi_mode = &edid_4k_modes[vic];
		unsigned int clock1, clock2;

		/* Make sure to also match alternate clocks */
		clock1 = (unsigned int)hdmi_mode->clock;
		clock2 = hdmi_mode_alternate_clock(hdmi_mode);

		if ((KHZ2PICOS(to_match->clock) == KHZ2PICOS(clock1) ||
		     KHZ2PICOS(to_match->clock) == KHZ2PICOS(clock2)) &&
		    drm_mode_match(to_match, hdmi_mode, match_flags))
			return vic;
	}
	return 0;
}

bool drm_valid_hdmi_vic(uint8_t vic)
{
	return vic > 0 && vic < ARRAY_SIZE(edid_4k_modes);
}

/* ---- the client-form interface ---------------------------------------------------
 *
 * What the EDID parser and the i915 driver have always called, on struct
 * drm_mode_modeinfo.  The results are unchanged where the old code was
 * right: same names, same refresh rounding, same table contents. */

/* The client-form fields a computation reads, into a core mode. */
static void umode_to_mode(struct drm_display_mode *out,
			  const struct drm_mode_modeinfo *in)
{
	mode_zero(out);
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
	out->type = (uint8_t)in->type;
}

/* A generated or table mode in the client form the old interface gave:
 * no picture-aspect bits, type DRIVER, the name and rate computed. */
static void mode_to_driver_umode(struct drm_mode_modeinfo *out,
				 const struct drm_display_mode *in)
{
	unsigned int i;

	for (i = 0; i < sizeof(*out); i++)
		((uint8_t *)out)[i] = 0;
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
	out->flags = in->flags & ~DRM_MODE_FLAG_PIC_AR_MASK;
	out->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_finish(out);
}

/**
 * drm_mode_finish - fill in vrefresh and the name from the timings
 *
 * A mode without totals keeps the vrefresh it has.  The name is the
 * mode's "<w>x<h>[i]".
 */
void drm_mode_finish(struct drm_mode_modeinfo *m)
{
	struct drm_display_mode dm;
	unsigned int i;

	umode_to_mode(&dm, m);
	if (m->htotal && m->vtotal)
		m->vrefresh = (uint32_t)drm_mode_vrefresh(&dm);
	drm_mode_set_name(&dm);
	for (i = 0; i < sizeof(m->name); i++)
		m->name[i] = dm.name[i];
}

/**
 * drm_mode_equal - same geometry and timing (client form)
 *
 * The clock exactly, the eight timings, interlace and the sync
 * polarities; the name, type, skew, vscan and the other flags are not
 * compared.
 */
int drm_mode_equal(const struct drm_mode_modeinfo *a,
		   const struct drm_mode_modeinfo *b)
{
	const uint32_t keep = DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_PHSYNC |
			      DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC |
			      DRM_MODE_FLAG_NVSYNC;
	struct drm_display_mode ma, mb;

	if (a->clock != b->clock)
		return 0;
	umode_to_mode(&ma, a);
	umode_to_mode(&mb, b);
	ma.hskew = mb.hskew = 0;
	ma.vscan = mb.vscan = 0;
	ma.flags &= keep;
	mb.flags &= keep;
	return drm_mode_match(&ma, &mb, DRM_MODE_MATCH_TIMINGS |
			      DRM_MODE_MATCH_FLAGS);
}


/* ---- the standard list ------------------------------------------------------- */

struct std_mode {
	uint32_t clock;
	uint16_t hd, hss, hse, ht;
	uint16_t vd, vss, vse, vt;
	uint32_t flags;
};

#define PH DRM_MODE_FLAG_PHSYNC
#define NH DRM_MODE_FLAG_NHSYNC
#define PV DRM_MODE_FLAG_PVSYNC
#define NV DRM_MODE_FLAG_NVSYNC

/* What a connector without an EDID (or with a range descriptor) is
 * offered, filtered by its limits (drm_connector_add_std_modes): DMT from
 * the 640x480 of every monitor to the 4K of a modern one, plus the CEA
 * 2160p timings; reduced-blanking entries marked RB in the comment.  Every
 * entry is a DMT or CEA timing, or (2560x1440, 3840x2160 RB) the CVT
 * reduced-blanking one; the host test checks each against the tables and
 * the formula.  The order is the order the modes are listed in. */
static const struct std_mode dmt[] = {
	{ 25175, 640, 656, 752, 800, 480, 490, 492, 525, NH | NV },
	{ 31500, 640, 664, 704, 832, 480, 489, 492, 520, NH | NV }, /* 72 */
	{ 31500, 640, 656, 720, 840, 480, 481, 484, 500, NH | NV }, /* 75 */
	{ 36000, 640, 696, 752, 832, 480, 481, 484, 509, NH | NV }, /* 85 */
	{ 36000, 800, 824, 896, 1024, 600, 601, 603, 625, PH | PV }, /* 56 */
	{ 40000, 800, 840, 968, 1056, 600, 601, 605, 628, PH | PV }, /* 60 */
	{ 50000, 800, 856, 976, 1040, 600, 637, 643, 666, PH | PV }, /* 72 */
	{ 49500, 800, 816, 896, 1056, 600, 601, 604, 625, PH | PV }, /* 75 */
	{ 56250, 800, 832, 896, 1048, 600, 601, 604, 631, PH | PV }, /* 85 */
	{ 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, NH | NV }, /* 60 */
	{ 75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, NH | NV }, /* 70 */
	{ 78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, PH | PV }, /* 75 */
	{ 94500, 1024, 1072, 1168, 1376, 768, 769, 772, 808, PH | PV }, /* 85 */
	{ 108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, PH | PV }, /* 75 */
	{ 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, PH | PV }, /* 60 */
	{ 68250, 1280, 1328, 1360, 1440, 768, 771, 778, 790, PH | NV }, /* 60 RB */
	{ 79500, 1280, 1344, 1472, 1664, 768, 771, 778, 798, NH | PV }, /* 60 */
	{ 102250, 1280, 1360, 1488, 1696, 768, 771, 778, 805, NH | PV }, /* 75 */
	{ 71000, 1280, 1328, 1360, 1440, 800, 803, 809, 823, PH | NV }, /* 60 RB */
	{ 83500, 1280, 1352, 1480, 1680, 800, 803, 809, 831, NH | PV }, /* 60 */
	{ 106500, 1280, 1360, 1488, 1696, 800, 803, 809, 838, NH | PV }, /* 75 */
	{ 108000, 1280, 1376, 1488, 1800, 960, 961, 964, 1000, PH | PV }, /* 60 */
	{ 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, PH | PV }, /* 60 */
	{ 135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, PH | PV }, /* 75 */
	{ 157500, 1280, 1344, 1504, 1728, 1024, 1025, 1028, 1072, PH | PV }, /* 85 */
	{ 85500, 1360, 1424, 1536, 1792, 768, 771, 777, 795, PH | PV }, /* 60 */
	{ 101000, 1400, 1448, 1480, 1560, 1050, 1053, 1057, 1080, PH | NV }, /* 60 RB */
	{ 121750, 1400, 1488, 1632, 1864, 1050, 1053, 1057, 1089, NH | PV }, /* 60 */
	{ 88750, 1440, 1488, 1520, 1600, 900, 903, 909, 926, PH | NV }, /* 60 RB */
	{ 106500, 1440, 1520, 1672, 1904, 900, 903, 909, 934, NH | PV }, /* 60 */
	{ 162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, PH | PV }, /* 60 */
	{ 119000, 1680, 1728, 1760, 1840, 1050, 1053, 1059, 1080, PH | NV }, /* 60 RB */
	{ 146250, 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, NH | PV }, /* 60 */
	{ 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, PH | PV }, /* 60 */
	{ 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, PH | NV }, /* 60 RB */
	{ 193250, 1920, 2056, 2256, 2592, 1200, 1203, 1209, 1245, NH | PV }, /* 60 */
	{ 234000, 1920, 2048, 2256, 2600, 1440, 1441, 1444, 1500, NH | PV }, /* 60 */
	{ 241500, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1481, PH | NV }, /* 60 RB */
	{ 268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646, PH | NV }, /* 60 RB */
	{ 348500, 2560, 2752, 3032, 3504, 1600, 1603, 1609, 1658, NH | PV }, /* 60 */
	{ 533250, 3840, 3888, 3920, 4000, 2160, 2163, 2168, 2222, PH | NV }, /* 60 RB */
	{ 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV }, /* 30 */
	{ 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, PH | PV }, /* 60 CEA */
};

int drm_mode_table_count(void)
{
	return (int)(sizeof(dmt) / sizeof(dmt[0]));
}

static void std_to_mode(const struct std_mode *s, struct drm_mode_modeinfo *m)
{
	unsigned int i;

	for (i = 0; i < sizeof(*m); i++)
		((uint8_t *)m)[i] = 0;
	m->clock = s->clock;
	m->hdisplay = s->hd;
	m->hsync_start = s->hss;
	m->hsync_end = s->hse;
	m->htotal = s->ht;
	m->vdisplay = s->vd;
	m->vsync_start = s->vss;
	m->vsync_end = s->vse;
	m->vtotal = s->vt;
	m->flags = s->flags;
	m->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_finish(m);
}

int drm_mode_table_get(int i, struct drm_mode_modeinfo *m)
{
	if (i < 0 || i >= drm_mode_table_count())
		return -1;
	std_to_mode(&dmt[i], m);
	return 0;
}

/* The video codes the client-form lookup answers for: the ones sinks
 * commonly list, which the EDID parser turns into modes and the HDMI
 * encoders match against.  The full set is drm_cea_mode_for_vic()'s. */
static const uint8_t cea_common_vics[] = {
	1, 2, 3, 4, 5, 16, 17, 18, 19, 20, 31, 32, 33, 34,
	60, 61, 62, 63, 64, 93, 94, 95, 96, 97, 98, 101, 102,
};

/**
 * drm_mode_cea_vic - a common CEA-861 video code's mode, client form
 *
 * Without the picture-aspect bits, type DRIVER.  0, or -1 for a code
 * outside the common set.
 */
int drm_mode_cea_vic(uint8_t vic, struct drm_mode_modeinfo *m)
{
	unsigned int i;

	for (i = 0; i < sizeof(cea_common_vics); i++) {
		if (cea_common_vics[i] == vic) {
			const struct drm_display_mode *cm = drm_cea_mode_for_vic(vic);

			if (!cm)
				return -1;
			mode_to_driver_umode(m, cm);
			return 0;
		}
	}
	return -1;
}

/* ---- CVT and GTF in the client form -------------------------------------------- */

/**
 * drm_mode_cvt - CVT timings of a geometry, client form
 * @rb: reduced blanking (v1)
 *
 * drm_cvt_mode(), progressive, no margins; refresh 0 means 60 Hz.  The
 * mode is type DRIVER with its name and refresh rate filled in; a geometry
 * the formula refuses gives an all-zero mode.
 */
void drm_mode_cvt(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh, int rb)
{
	struct drm_display_mode dm;
	unsigned int i;

	if (!vrefresh)
		vrefresh = 60;
	if (w > 0xffff || h > 0xffff || vrefresh > 0xffff ||
	    drm_cvt_mode(&dm, (int)w, (int)h, (int)vrefresh, !!rb, false,
			 false)) {
		for (i = 0; i < sizeof(*m); i++)
			((uint8_t *)m)[i] = 0;
		return;
	}
	mode_to_driver_umode(m, &dm);
}

/**
 * drm_mode_gtf - GTF timings of a geometry, client form
 *
 * drm_gtf_mode() with the default parameters, progressive, no margins;
 * refresh 0 means 60 Hz.  As drm_mode_cvt() otherwise.
 */
void drm_mode_gtf(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		  uint32_t vrefresh)
{
	struct drm_display_mode dm;
	unsigned int i;

	if (!vrefresh)
		vrefresh = 60;
	if (w > 0xffff || h > 0xffff || vrefresh > 0xffff ||
	    drm_gtf_mode(&dm, (int)w, (int)h, (int)vrefresh, false, 0)) {
		for (i = 0; i < sizeof(*m); i++)
			((uint8_t *)m)[i] = 0;
		return;
	}
	mode_to_driver_umode(m, &dm);
}

int drm_mode_in_range(const struct drm_mode_modeinfo *m,
		      const struct drm_edid_range *r, uint32_t max_clock_khz,
		      uint32_t max_w, uint32_t max_h)
{
	if (max_clock_khz && m->clock > max_clock_khz)
		return 0;
	if (max_w && m->hdisplay > max_w)
		return 0;
	if (max_h && m->vdisplay > max_h)
		return 0;
	if (r) {
		uint32_t hfreq_khz = m->htotal ? (m->clock / m->htotal) : 0;
		if (r->max_clock_khz && m->clock > r->max_clock_khz)
			return 0;
		if (r->min_vrefresh && m->vrefresh < r->min_vrefresh)
			return 0;
		if (r->max_vrefresh && m->vrefresh > r->max_vrefresh)
			return 0;
		if (r->min_hfreq && hfreq_khz < r->min_hfreq)
			return 0;
		if (r->max_hfreq && hfreq_khz > r->max_hfreq)
			return 0;
	}
	return 1;
}
