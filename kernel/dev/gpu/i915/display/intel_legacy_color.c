// LikeOS -- colour management of the Intel displays before DDI.
//
// After the planes are blended, a pipe's pixels pass through its colour
// stages, which differ from family to family:
//
//   gen2-4:      the pipe palette -- 256 entries of 8 bits, or (mobile gen2
//                and gen4) 129 entries of 10 bits, interpolated
//   Valleyview:  the WGC matrix (s2.10), then the palette as on gen4
//   Cherryview:  the CGM unit -- degamma (65 x 14 bits), matrix (s4.12),
//                gamma (257 x 10 bits) -- then the WGC and the palette;
//                the CGM matrix replaces the WGC one, and the palette is
//                kept for the 8-bit table only
//   Ironlake, Sandy Bridge: the pipe CSC and one table, 8-bit (256) or
//                10-bit (1024, PREC_PALETTE)
//   Ivy Bridge:  the pipe CSC and one 1024-entry 10-bit table that can be
//                split into a degamma and a gamma half of 512
//
// A crtc offers GAMMA_LUT at the size of the part's precise table (the
// 256-entry table SETGAMMA makes is taken on every part, as the 8-bit
// palette), DEGAMMA_LUT where there is one, and CTM from Ironlake and
// Valleyview on.  With no table but a 256-entry one, no degamma table and
// no matrix, a pipe runs the 8-bit palette exactly as it always has: the
// palette written from the crtc state's 256-entry table and the pipe gamma
// enabled in the planes.  Everything else is the per-family state below,
// computed from the crtc state, checked in the atomic check and programmed
// in the commit.
//
// The crtcs of this display are not its pipes -- a pipe is chosen at the
// mode set -- so a crtc offers what every pipe of the part can load.  On
// gen3 the 10-bit palette exists on pipe B only; the crtcs of a gen3 part
// therefore take the 8-bit table alone.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2016 Intel Corporation
// Portions Copyright (C) 2023 Intel Corporation

#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/i915/intel_legacy_color.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>

/* ---- registers --------------------------------------------------------------- */

/* The fields of an 8-bit palette entry (and of each half of the gen4
 * 10.6 format). */
#define LCOL_PALETTE_RED_SHIFT 16
#define LCOL_PALETTE_GREEN_SHIFT 8
#define LCOL_PALETTE_BLUE_SHIFT 0

/* PIPECONF's gamma mode field */
#define LCOL_GAMMA_MODE_8BIT 0u
#define LCOL_GAMMA_MODE_10BIT 1u
#define LCOL_GAMMA_MODE_SPLIT 3u /* Ivy Bridge */
#define LCOL_PIPECONF_GAMMA_MODE(x) ((uint32_t)(x) << 24)

/* Ironlake / Sandy Bridge: the 10-bit table */
#define LCOL_PREC_PALETTE(p, i) (0x4b000u + (uint32_t)(p) * 0x1000u + (uint32_t)(i) * 4u)
/* Ivy Bridge: the indexed 10-bit table */
#define LCOL_PREC_PAL_INDEX(p) (0x4a400u + (uint32_t)(p) * 0x800u)
#define LCOL_PAL_PREC_SPLIT_MODE (1u << 31)
#define LCOL_PAL_PREC_INDEX_VALUE(x) ((uint32_t)(x) & 0x3ffu)
#define LCOL_PREC_PAL_DATA(p) (0x4a404u + (uint32_t)(p) * 0x800u)
#define LCOL_PREC_PAL_EXT_GC_MAX(p, i) (0x4a420u + (uint32_t)(p) * 0x800u + (uint32_t)(i) * 4u)
/* the 10-bit entry */
#define LCOL_PREC_10_RED_SHIFT 20
#define LCOL_PREC_10_GREEN_SHIFT 10
#define LCOL_PREC_10_BLUE_SHIFT 0

/* Ironlake to Ivy Bridge: the pipe CSC */
#define LCOL_CSC_BASE(p) (0x49000u + (uint32_t)(p) * 0x100u)
#define LCOL_CSC_COEFF_RY_GY(p) (LCOL_CSC_BASE(p) + 0x10u)
#define LCOL_CSC_COEFF_BY(p) (LCOL_CSC_BASE(p) + 0x14u)
#define LCOL_CSC_COEFF_RU_GU(p) (LCOL_CSC_BASE(p) + 0x18u)
#define LCOL_CSC_COEFF_BU(p) (LCOL_CSC_BASE(p) + 0x1cu)
#define LCOL_CSC_COEFF_RV_GV(p) (LCOL_CSC_BASE(p) + 0x20u)
#define LCOL_CSC_COEFF_BV(p) (LCOL_CSC_BASE(p) + 0x24u)
#define LCOL_CSC_MODE(p) (LCOL_CSC_BASE(p) + 0x28u)
#define LCOL_CSC_POSITION_BEFORE_GAMMA (1u << 1)
#define LCOL_CSC_MODE_YUV_TO_RGB (1u << 0) /* Ironlake, Sandy Bridge */
#define LCOL_CSC_PREOFF_HI(p) (LCOL_CSC_BASE(p) + 0x30u)
#define LCOL_CSC_PREOFF_ME(p) (LCOL_CSC_BASE(p) + 0x34u)
#define LCOL_CSC_PREOFF_LO(p) (LCOL_CSC_BASE(p) + 0x38u)
#define LCOL_CSC_POSTOFF_HI(p) (LCOL_CSC_BASE(p) + 0x40u) /* Ivy Bridge */
#define LCOL_CSC_POSTOFF_ME(p) (LCOL_CSC_BASE(p) + 0x44u)
#define LCOL_CSC_POSTOFF_LO(p) (LCOL_CSC_BASE(p) + 0x48u)

/* Valleyview: the WGC matrix (display-relative, transcoder spacing) */
#define LCOL_WGC_C01_C00(d, p) (0x600b0u + LG_TRANS_OFF(d, p))
#define LCOL_WGC_C02(d, p) (0x600b4u + LG_TRANS_OFF(d, p))
#define LCOL_WGC_C11_C10(d, p) (0x600b8u + LG_TRANS_OFF(d, p))
#define LCOL_WGC_C12(d, p) (0x600bcu + LG_TRANS_OFF(d, p))
#define LCOL_WGC_C21_C20(d, p) (0x600c0u + LG_TRANS_OFF(d, p))
#define LCOL_WGC_C22(d, p) (0x600c4u + LG_TRANS_OFF(d, p))

/* Cherryview: the CGM unit (display-relative; pipes 0x2000 apart) */
#define LCOL_CGM_OFF(p) ((uint32_t)(p) * 0x2000u)
#define LCOL_CGM_CSC_COEFF01(p) (0x67900u + LCOL_CGM_OFF(p))
#define LCOL_CGM_CSC_COEFF23(p) (0x67904u + LCOL_CGM_OFF(p))
#define LCOL_CGM_CSC_COEFF45(p) (0x67908u + LCOL_CGM_OFF(p))
#define LCOL_CGM_CSC_COEFF67(p) (0x6790cu + LCOL_CGM_OFF(p))
#define LCOL_CGM_CSC_COEFF8(p) (0x67910u + LCOL_CGM_OFF(p))
#define LCOL_CGM_DEGAMMA(p, i, w) (0x66000u + LCOL_CGM_OFF(p) + (uint32_t)(i) * 8u + (uint32_t)(w) * 4u)
#define LCOL_CGM_GAMMA(p, i, w) (0x67000u + LCOL_CGM_OFF(p) + (uint32_t)(i) * 8u + (uint32_t)(w) * 4u)
#define LCOL_CGM_MODE(p) (0x67a00u + LCOL_CGM_OFF(p))
#define LCOL_CGM_MODE_GAMMA (1u << 2)
#define LCOL_CGM_MODE_CSC (1u << 1)
#define LCOL_CGM_MODE_DEGAMMA (1u << 0)

/* ---- what each part has ----------------------------------------------------------- */

#define LCOL_LEGACY_LUT_LENGTH 256

struct lcol_caps {
	uint32_t gamma_size; /* GAMMA_LUT entries besides the 256 legacy ones */
	uint32_t degamma_size; /* 0: none */
	int has_ctm;
	uint32_t gamma_tests, degamma_tests; /* drm_color_lut_check() */
};

static void caps_for(int platform, struct lcol_caps *c)
{
	mm_memset(c, 0, sizeof(*c));
	c->gamma_size = LCOL_LEGACY_LUT_LENGTH;
	switch (platform) {
	case I915_PLATFORM_I830:
	case I915_PLATFORM_I85X:
	case I915_PLATFORM_I965G:
	case I915_PLATFORM_I965GM:
	case I915_PLATFORM_G45:
	case I915_PLATFORM_GM45:
	case I915_PLATFORM_VALLEYVIEW:
		/* the 10-bit interpolated palette */
		c->gamma_size = 129;
		c->gamma_tests = DRM_COLOR_LUT_NON_DECREASING;
		c->has_ctm = platform == I915_PLATFORM_VALLEYVIEW;
		break;
	case I915_PLATFORM_I845G:
	case I915_PLATFORM_I865G:
	case I915_PLATFORM_I915G:
	case I915_PLATFORM_I915GM:
	case I915_PLATFORM_I945G:
	case I915_PLATFORM_I945GM:
	case I915_PLATFORM_G33:
	case I915_PLATFORM_PINEVIEW:
		/* The 8-bit palette only.  The mobile gen3 parts have the
		 * 10-bit palette on pipe B, but which pipe a crtc gets is
		 * decided at the mode set: the table the crtc offers must
		 * load on either. */
		break;
	case I915_PLATFORM_IRONLAKE:
	case I915_PLATFORM_SANDYBRIDGE:
		c->gamma_size = 1024;
		c->has_ctm = 1;
		break;
	case I915_PLATFORM_IVYBRIDGE:
		c->gamma_size = 1024;
		c->degamma_size = 1024;
		c->has_ctm = 1;
		break;
	case I915_PLATFORM_CHERRYVIEW:
		c->gamma_size = 257;
		c->degamma_size = 65;
		c->has_ctm = 1;
		c->gamma_tests = DRM_COLOR_LUT_NON_DECREASING;
		c->degamma_tests = DRM_COLOR_LUT_NON_DECREASING;
		break;
	default:
		break;
	}
#if !DRM_COLOR_LEGACY_GAMMA_SPLIT
	/* The core ties GAMMA_LUT_SIZE to the legacy table's size: the
	 * 256-entry table alone. */
	c->gamma_size = LCOL_LEGACY_LUT_LENGTH;
	c->gamma_tests = 0;
#endif
}

/* ---- the state of a pipe's colour stages ---------------------------------------- */

struct lcol_state {
	/* Nothing but the 8-bit palette from the state's 256-entry table:
	 * the path every pipe ran before colour management. */
	uint8_t legacy;
	uint8_t gamma_enable; /* the pipe palette is in the path (plane bits) */
	uint8_t csc_enable; /* Ironlake to Ivy Bridge: the pipe CSC (plane bits) */
	uint8_t wgc_enable; /* Valleyview: the WGC matrix (PIPECONF) */
	uint32_t gamma_mode; /* LCOL_GAMMA_MODE_* */
	uint32_t csc_mode; /* Ironlake to Ivy Bridge: PIPE_CSC_MODE */
	uint32_t cgm_mode; /* Cherryview: CGM_PIPE_MODE */
	/* which table goes before and after the matrix (Ironlake to Ivy
	 * Bridge): 0 none, 1 the degamma table, 2 the gamma table */
	uint8_t pre_lut, post_lut;
	uint16_t coeff[9]; /* the matrix in the hardware's format */
	uint16_t preoff[3], postoff[3];
};

/* What each pipe was programmed with: the legacy state (the plain 8-bit
 * palette) until a mode set computes another, and again once the pipe
 * goes off (lg_color_pipe_reset()). */
static struct lcol_state g_state[LG_MAX_PIPES] = {
	[0 ... LG_MAX_PIPES - 1] = { .legacy = 1, .gamma_enable = 1 },
};

static int lut_len(const struct drm_blob *b)
{
	return b ? drm_color_lut_size(b) : 0;
}

static int lut_is_legacy(const struct drm_blob *b)
{
	return b && lut_len(b) == LCOL_LEGACY_LUT_LENGTH;
}

static void state_legacy(struct lcol_state *s)
{
	mm_memset(s, 0, sizeof(*s));
	s->legacy = 1;
	s->gamma_enable = 1;
	s->gamma_mode = LCOL_GAMMA_MODE_8BIT;
}

void lg_color_pipe_reset(struct lg_display *d, int pipe)
{
	(void)d;
	if (pipe >= 0 && pipe < LG_MAX_PIPES)
		state_legacy(&g_state[pipe]);
}

/* ---- the matrix ----------------------------------------------------------------- */

#define CTM_COEFF_SIGN (1ULL << 63)
#define CTM_COEFF_1_0 (1ULL << 32)
#define CTM_COEFF_2_0 (CTM_COEFF_1_0 << 1)
#define CTM_COEFF_4_0 (CTM_COEFF_2_0 << 1)
#define CTM_COEFF_0_5 (CTM_COEFF_1_0 >> 1)
#define CTM_COEFF_0_25 (CTM_COEFF_0_5 >> 1)
#define CTM_COEFF_0_125 (CTM_COEFF_0_25 >> 1)
#define CTM_COEFF_NEGATIVE(coeff) (((coeff) & CTM_COEFF_SIGN) != 0)
#define CTM_COEFF_ABS(coeff) ((coeff) & (CTM_COEFF_SIGN - 1))

/* The Ironlake CSC coefficient: a 9-bit mantissa window that slides with
 * the coefficient's size, written from bit 3, rounded. */
static uint16_t ilk_csc_coeff_fp(uint64_t coeff, int fbits)
{
	uint64_t v = (coeff >> (32 - fbits - 3)) + 4;

	if (v > 0xfff)
		v = 0xfff;
	return (uint16_t)(v & 0xff8);
}

#define ILK_CSC_COEFF_1_0 0x7800

/* The client's S31.32 matrix as the pipe CSC takes it (full range: the
 * parts before Haswell select limited range in PIPECONF instead). */
static void ilk_csc_convert_ctm(const struct drm_color_ctm *ctm, struct lcol_state *s)
{
	mm_memset(s->preoff, 0, sizeof(s->preoff));
	mm_memset(s->postoff, 0, sizeof(s->postoff));
	for (int i = 0; i < 9; i++) {
		uint64_t input = ctm->matrix[i];
		uint64_t abs_coeff = ((1ULL << 63) - 1) & input;
		uint16_t c = 0;

		/* the most the hardware can hold */
		if (abs_coeff > CTM_COEFF_4_0 - 1)
			abs_coeff = CTM_COEFF_4_0 - 1;
		if (CTM_COEFF_NEGATIVE(input))
			c |= 1u << 15;
		if (abs_coeff < CTM_COEFF_0_125)
			c |= (3u << 12) | ilk_csc_coeff_fp(abs_coeff, 12);
		else if (abs_coeff < CTM_COEFF_0_25)
			c |= (2u << 12) | ilk_csc_coeff_fp(abs_coeff, 11);
		else if (abs_coeff < CTM_COEFF_0_5)
			c |= (1u << 12) | ilk_csc_coeff_fp(abs_coeff, 10);
		else if (abs_coeff < CTM_COEFF_1_0)
			c |= ilk_csc_coeff_fp(abs_coeff, 9);
		else if (abs_coeff < CTM_COEFF_2_0)
			c |= (7u << 12) | ilk_csc_coeff_fp(abs_coeff, 8);
		else
			c |= (6u << 12) | ilk_csc_coeff_fp(abs_coeff, 7);
		s->coeff[i] = c;
	}
}

/* An S31.32 coefficient as a two's complement s(int_bits).(frac_bits)
 * number, rounded and clamped. */
static uint16_t ctm_to_twos_complement(uint64_t coeff, int int_bits, int frac_bits)
{
	int64_t c = (int64_t)CTM_COEFF_ABS(coeff);
	int64_t lo, hi;

	/* an extra bit for the rounding */
	c >>= 32 - frac_bits - 1;
	c = (c + 1) >> 1;
	if (CTM_COEFF_NEGATIVE(coeff))
		c = -c;
	if (int_bits < 1)
		int_bits = 1;
	lo = -(int64_t)(1LL << (int_bits + frac_bits - 1));
	hi = (int64_t)((1LL << (int_bits + frac_bits - 1)) - 1);
	if (c < lo)
		c = lo;
	if (c > hi)
		c = hi;
	return (uint16_t)((uint64_t)c & ((1ULL << (int_bits + frac_bits)) - 1));
}

#define CHV_CGM_CSC_COEFF_1_0 (1u << 12)

/* ---- computing a pipe's state ------------------------------------------------- */

/* The state of the colour stages for `cs' on this part.  Pure: the check
 * and the commit compute the same. */
static void compute_state(struct lg_display *d, const struct drm_crtc_state *cs,
			  struct lcol_state *s)
{
	struct lcol_caps caps;
	const struct drm_blob *gamma = cs->gamma_lut;
	const struct drm_blob *degamma = cs->degamma_lut;
	const struct drm_blob *ctm = cs->ctm;

	caps_for(d->platform, &caps);
	/* what the part does not offer is not there (a blob of a property
	 * the crtc does not list cannot have been set) */
	if (!caps.degamma_size)
		degamma = NULL;
	if (!caps.has_ctm)
		ctm = NULL;

	state_legacy(s);
	if (!degamma && !ctm && (!gamma || lut_is_legacy(gamma))) {
		/* Cherryview keeps the CGM matrix in the path all the time
		 * (identity here): switching it on or off outside the short
		 * window between the start of the vblank and the frame start
		 * underruns. */
		if (d->is_chv) {
			s->cgm_mode = LCOL_CGM_MODE_CSC;
			s->coeff[0] = s->coeff[4] = s->coeff[8] = CHV_CGM_CSC_COEFF_1_0;
		}
		return;
	}
	s->legacy = 0;

	if (d->is_chv) {
		/* CGM degamma -> CGM matrix -> CGM gamma -> (WGC bypassed)
		 * -> pipe palette; the palette serves the 8-bit table only */
		s->gamma_mode = LCOL_GAMMA_MODE_8BIT;
		s->gamma_enable = !(gamma && !lut_is_legacy(gamma));
		if (degamma)
			s->cgm_mode |= LCOL_CGM_MODE_DEGAMMA;
		if (gamma && !lut_is_legacy(gamma))
			s->cgm_mode |= LCOL_CGM_MODE_GAMMA;
		s->cgm_mode |= LCOL_CGM_MODE_CSC;
		if (ctm) {
			const struct drm_color_ctm *m = (const struct drm_color_ctm *)ctm->data;
			for (int i = 0; i < 9; i++)
				s->coeff[i] = ctm_to_twos_complement(m->matrix[i], 4, 12);
		} else {
			s->coeff[0] = s->coeff[4] = s->coeff[8] = CHV_CGM_CSC_COEFF_1_0;
		}
		return;
	}
	if (d->gmch) {
		/* gen2-4 and Valleyview: the palette, 8 or 10 bits; on
		 * Valleyview the WGC matrix before it */
		s->gamma_mode = gamma && !lut_is_legacy(gamma) ? LCOL_GAMMA_MODE_10BIT :
								 LCOL_GAMMA_MODE_8BIT;
		if (d->is_vlv && ctm) {
			const struct drm_color_ctm *m = (const struct drm_color_ctm *)ctm->data;
			s->wgc_enable = 1;
			for (int i = 0; i < 9; i++)
				s->coeff[i] = ctm_to_twos_complement(m->matrix[i], 2, 10);
		}
		return;
	}
	/* Ironlake to Ivy Bridge (RGB output only; limited range is
	 * PIPECONF's) */
	s->csc_enable = ctm != NULL;
	if (d->is_ivb && degamma && gamma)
		s->gamma_mode = LCOL_GAMMA_MODE_SPLIT;
	else if ((!gamma && !degamma) || lut_is_legacy(gamma))
		s->gamma_mode = LCOL_GAMMA_MODE_8BIT;
	else
		s->gamma_mode = LCOL_GAMMA_MODE_10BIT;
	/* The matrix before the table, unless the table is a degamma
	 * table (then after it). */
	if (d->is_ivb)
		s->csc_mode = degamma ? 0 : LCOL_CSC_POSITION_BEFORE_GAMMA;
	else
		s->csc_mode = degamma ? LCOL_CSC_MODE_YUV_TO_RGB :
					LCOL_CSC_MODE_YUV_TO_RGB | LCOL_CSC_POSITION_BEFORE_GAMMA;
	if (degamma || (s->csc_mode & LCOL_CSC_POSITION_BEFORE_GAMMA)) {
		s->pre_lut = degamma ? 1 : 0;
		s->post_lut = gamma ? 2 : 0;
	} else {
		s->pre_lut = gamma ? 2 : 0;
		s->post_lut = 0;
	}
	if (ctm)
		ilk_csc_convert_ctm((const struct drm_color_ctm *)ctm->data, s);
}

/* ---- the check ----------------------------------------------------------------- */

/* Gen2 10-bit palette: the slope to the last entry has 7 bits. */
static int i9xx_lut_10_diff(uint16_t a, uint16_t b)
{
	return (int)drm_color_lut_extract(a, 10) - (int)drm_color_lut_extract(b, 10);
}

static int i9xx_check_lut_10(const struct drm_blob *blob)
{
	const struct drm_color_lut *lut = drm_color_lut_data(blob);
	int n = lut_len(blob);
	const struct drm_color_lut *a = &lut[n - 2];
	const struct drm_color_lut *b = &lut[n - 1];

	if (i9xx_lut_10_diff(b->red, a->red) > 0x7f ||
	    i9xx_lut_10_diff(b->green, a->green) > 0x7f ||
	    i9xx_lut_10_diff(b->blue, a->blue) > 0x7f)
		return -EINVAL;
	return 0;
}

int lg_color_check(struct lg_display *d, const struct drm_crtc_state *cs)
{
	struct lcol_caps caps;
	struct lcol_state s;
	const struct drm_blob *gamma = cs->gamma_lut;
	const struct drm_blob *degamma = cs->degamma_lut;

	caps_for(d->platform, &caps);
	if (!caps.degamma_size)
		degamma = NULL;
	if (degamma && lut_len(degamma) != (int)caps.degamma_size)
		return -EINVAL;
	if (gamma) {
		int expected = lut_is_legacy(gamma) ? LCOL_LEGACY_LUT_LENGTH : (int)caps.gamma_size;
		if (lut_len(gamma) != expected)
			return -EINVAL;
	}
	if (drm_color_lut_check(degamma, caps.degamma_tests) ||
	    drm_color_lut_check(gamma, lut_is_legacy(gamma) ? 0 : caps.gamma_tests))
		return -EINVAL;
	/* Ironlake and Sandy Bridge have one table, before or after the
	 * matrix */
	if ((d->is_ilk || d->is_snb) && degamma && gamma)
		return -EINVAL;
	compute_state(d, cs, &s);
	if (d->ver < 4 && s.gamma_mode == LCOL_GAMMA_MODE_10BIT && i9xx_check_lut_10(gamma))
		return -EINVAL;
	return 0;
}

/* ---- loading the tables -------------------------------------------------------- */

/* The 8-bit palette is written by lg_gamma_load() from the crtc state's
 * 256-entry table (which the core keeps equal to a 256-entry GAMMA_LUT),
 * as it always was. */

/* gen2/3 10-bit format, "even" dword: the low 8 bits */
static uint32_t i9xx_lut_10_ldw_1(uint16_t a)
{
	return drm_color_lut_extract(a, 10) & 0xff;
}

static uint32_t i9xx_lut_10_ldw(const struct drm_color_lut *c)
{
	return (i9xx_lut_10_ldw_1(c[0].red) << LCOL_PALETTE_RED_SHIFT) |
	       (i9xx_lut_10_ldw_1(c[0].green) << LCOL_PALETTE_GREEN_SHIFT) |
	       (i9xx_lut_10_ldw_1(c[0].blue) << LCOL_PALETTE_BLUE_SHIFT);
}

/* "odd" dword: the high 2 bits and the slope to the next entry
 * (b = a + 8 * m * 2^-e) */
static uint32_t i9xx_lut_10_udw_1(uint16_t a16, uint16_t b16)
{
	int a = (int)drm_color_lut_extract(a16, 10);
	int b = (int)drm_color_lut_extract(b16, 10);
	int mantissa = b - a;
	uint32_t exponent = 3;

	if (mantissa < 0)
		mantissa = 0;
	if (mantissa > 0x7f)
		mantissa = 0x7f;
	while (mantissa > 0xf) {
		mantissa >>= 1;
		exponent--;
	}
	return (exponent << 6) | ((uint32_t)mantissa << 2) | ((uint32_t)a >> 8);
}

static uint32_t i9xx_lut_10_udw(const struct drm_color_lut *c)
{
	return (i9xx_lut_10_udw_1(c[0].red, c[1].red) << LCOL_PALETTE_RED_SHIFT) |
	       (i9xx_lut_10_udw_1(c[0].green, c[1].green) << LCOL_PALETTE_GREEN_SHIFT) |
	       (i9xx_lut_10_udw_1(c[0].blue, c[1].blue) << LCOL_PALETTE_BLUE_SHIFT);
}

static void i9xx_load_lut_10(struct lg_display *d, int pipe, const struct drm_blob *blob)
{
	const struct drm_color_lut *lut = drm_color_lut_data(blob);
	int n = lut_len(blob);

	for (int i = 0; i < n - 1; i++) {
		lg_wr(d, PALETTE(d, pipe, 2 * i + 0), i9xx_lut_10_ldw(&lut[i]));
		lg_wr(d, PALETTE(d, pipe, 2 * i + 1), i9xx_lut_10_udw(&lut[i]));
	}
}

/* gen4 and later "10.6" interpolated format: low and high byte of each
 * 16-bit value, the last entry in PIPEGCMAX */
static uint32_t i965_lut_10p6_ldw(const struct drm_color_lut *c)
{
	return ((uint32_t)(c->red & 0xff) << LCOL_PALETTE_RED_SHIFT) |
	       ((uint32_t)(c->green & 0xff) << LCOL_PALETTE_GREEN_SHIFT) |
	       ((uint32_t)(c->blue & 0xff) << LCOL_PALETTE_BLUE_SHIFT);
}

static uint32_t i965_lut_10p6_udw(const struct drm_color_lut *c)
{
	return ((uint32_t)(c->red >> 8) << LCOL_PALETTE_RED_SHIFT) |
	       ((uint32_t)(c->green >> 8) << LCOL_PALETTE_GREEN_SHIFT) |
	       ((uint32_t)(c->blue >> 8) << LCOL_PALETTE_BLUE_SHIFT);
}

static void i965_load_lut_10p6(struct lg_display *d, int pipe, const struct drm_blob *blob)
{
	const struct drm_color_lut *lut = drm_color_lut_data(blob);
	int n = lut_len(blob);
	int i;

	for (i = 0; i < n - 1; i++) {
		lg_wr(d, PALETTE(d, pipe, 2 * i + 0), i965_lut_10p6_ldw(&lut[i]));
		lg_wr(d, PALETTE(d, pipe, 2 * i + 1), i965_lut_10p6_udw(&lut[i]));
	}
	lg_wr(d, PIPEGCMAX(d, pipe, 0), lut[i].red);
	lg_wr(d, PIPEGCMAX(d, pipe, 1), lut[i].green);
	lg_wr(d, PIPEGCMAX(d, pipe, 2), lut[i].blue);
}

static uint32_t ilk_lut_10(const struct drm_color_lut *c)
{
	return (drm_color_lut_extract(c->red, 10) << LCOL_PREC_10_RED_SHIFT) |
	       (drm_color_lut_extract(c->green, 10) << LCOL_PREC_10_GREEN_SHIFT) |
	       (drm_color_lut_extract(c->blue, 10) << LCOL_PREC_10_BLUE_SHIFT);
}

static void ilk_load_lut_10(struct lg_display *d, int pipe, const struct drm_blob *blob)
{
	const struct drm_color_lut *lut = drm_color_lut_data(blob);
	int n = lut_len(blob);

	for (int i = 0; i < n; i++)
		lg_wr(d, LCOL_PREC_PALETTE(pipe, i), ilk_lut_10(&lut[i]));
}

/* Ivy Bridge: entry by entry through the index register (its auto
 * increment does not work there); `out_size' entries resampled from the
 * blob (512 for each half of the split table). */
static void ivb_load_lut_10(struct lg_display *d, int pipe, const struct drm_blob *blob,
			    int out_size, uint32_t prec_index)
{
	const struct drm_color_lut *lut = drm_color_lut_data(blob);
	int n = lut_len(blob);

	for (int i = 0; i < out_size; i++) {
		const struct drm_color_lut *e =
			out_size == n ? &lut[i] : &lut[i * (n - 1) / (out_size - 1)];
		lg_wr(d, LCOL_PREC_PAL_INDEX(pipe), prec_index + (uint32_t)i);
		lg_wr(d, LCOL_PREC_PAL_DATA(pipe), ilk_lut_10(e));
	}
	/* back to 0, or the 8-bit palette is not written properly */
	lg_wr(d, LCOL_PREC_PAL_INDEX(pipe), LCOL_PAL_PREC_INDEX_VALUE(0));
}

/* values above 1.0 clamped */
static void ivb_load_lut_ext_max(struct lg_display *d, int pipe)
{
	lg_wr(d, LCOL_PREC_PAL_EXT_GC_MAX(pipe, 0), 1u << 16);
	lg_wr(d, LCOL_PREC_PAL_EXT_GC_MAX(pipe, 1), 1u << 16);
	lg_wr(d, LCOL_PREC_PAL_EXT_GC_MAX(pipe, 2), 1u << 16);
}

static void ilk_load_csc(struct lg_display *d, int pipe, const struct lcol_state *s)
{
	lg_wr(d, LCOL_CSC_PREOFF_HI(pipe), s->preoff[0]);
	lg_wr(d, LCOL_CSC_PREOFF_ME(pipe), s->preoff[1]);
	lg_wr(d, LCOL_CSC_PREOFF_LO(pipe), s->preoff[2]);
	lg_wr(d, LCOL_CSC_COEFF_RY_GY(pipe), (uint32_t)s->coeff[0] << 16 | s->coeff[1]);
	lg_wr(d, LCOL_CSC_COEFF_BY(pipe), (uint32_t)s->coeff[2] << 16);
	lg_wr(d, LCOL_CSC_COEFF_RU_GU(pipe), (uint32_t)s->coeff[3] << 16 | s->coeff[4]);
	lg_wr(d, LCOL_CSC_COEFF_BU(pipe), (uint32_t)s->coeff[5] << 16);
	lg_wr(d, LCOL_CSC_COEFF_RV_GV(pipe), (uint32_t)s->coeff[6] << 16 | s->coeff[7]);
	lg_wr(d, LCOL_CSC_COEFF_BV(pipe), (uint32_t)s->coeff[8] << 16);
	if (!d->is_ivb)
		return;
	lg_wr(d, LCOL_CSC_POSTOFF_HI(pipe), s->postoff[0]);
	lg_wr(d, LCOL_CSC_POSTOFF_ME(pipe), s->postoff[1]);
	lg_wr(d, LCOL_CSC_POSTOFF_LO(pipe), s->postoff[2]);
}

static void vlv_load_wgc_csc(struct lg_display *d, int pipe, const struct lcol_state *s)
{
	lg_wr(d, LCOL_WGC_C01_C00(d, pipe), (uint32_t)s->coeff[1] << 16 | s->coeff[0]);
	lg_wr(d, LCOL_WGC_C02(d, pipe), s->coeff[2]);
	lg_wr(d, LCOL_WGC_C11_C10(d, pipe), (uint32_t)s->coeff[4] << 16 | s->coeff[3]);
	lg_wr(d, LCOL_WGC_C12(d, pipe), s->coeff[5]);
	lg_wr(d, LCOL_WGC_C21_C20(d, pipe), (uint32_t)s->coeff[7] << 16 | s->coeff[6]);
	lg_wr(d, LCOL_WGC_C22(d, pipe), s->coeff[8]);
}

static void chv_load_cgm_csc(struct lg_display *d, int pipe, const struct lcol_state *s)
{
	lg_wr(d, LCOL_CGM_CSC_COEFF01(pipe), (uint32_t)s->coeff[1] << 16 | s->coeff[0]);
	lg_wr(d, LCOL_CGM_CSC_COEFF23(pipe), (uint32_t)s->coeff[3] << 16 | s->coeff[2]);
	lg_wr(d, LCOL_CGM_CSC_COEFF45(pipe), (uint32_t)s->coeff[5] << 16 | s->coeff[4]);
	lg_wr(d, LCOL_CGM_CSC_COEFF67(pipe), (uint32_t)s->coeff[7] << 16 | s->coeff[6]);
	lg_wr(d, LCOL_CGM_CSC_COEFF8(pipe), s->coeff[8]);
}

static void chv_load_cgm_degamma(struct lg_display *d, int pipe, const struct drm_blob *blob)
{
	const struct drm_color_lut *lut = drm_color_lut_data(blob);
	int n = lut_len(blob);

	for (int i = 0; i < n; i++) {
		lg_wr(d, LCOL_CGM_DEGAMMA(pipe, i, 0),
		      drm_color_lut_extract(lut[i].green, 14) << 16 |
			      drm_color_lut_extract(lut[i].blue, 14));
		lg_wr(d, LCOL_CGM_DEGAMMA(pipe, i, 1), drm_color_lut_extract(lut[i].red, 14));
	}
}

static void chv_load_cgm_gamma(struct lg_display *d, int pipe, const struct drm_blob *blob)
{
	const struct drm_color_lut *lut = drm_color_lut_data(blob);
	int n = lut_len(blob);

	for (int i = 0; i < n; i++) {
		lg_wr(d, LCOL_CGM_GAMMA(pipe, i, 0),
		      drm_color_lut_extract(lut[i].green, 10) << 16 |
			      drm_color_lut_extract(lut[i].blue, 10));
		lg_wr(d, LCOL_CGM_GAMMA(pipe, i, 1), drm_color_lut_extract(lut[i].red, 10));
	}
}

static const struct drm_blob *lut_of(const struct drm_crtc_state *cs, uint8_t which)
{
	return which == 1 ? cs->degamma_lut : which == 2 ? cs->gamma_lut : NULL;
}

/* Every table and matrix of state `s' from `cs' into the pipe. */
static void load_state(struct lg_display *d, int pipe, const struct lcol_state *s,
		       const struct drm_crtc_state *cs)
{
	if (d->is_chv) {
		if (s->cgm_mode & LCOL_CGM_MODE_CSC)
			chv_load_cgm_csc(d, pipe, s);
		if ((s->cgm_mode & LCOL_CGM_MODE_DEGAMMA) && cs->degamma_lut)
			chv_load_cgm_degamma(d, pipe, cs->degamma_lut);
		if ((s->cgm_mode & LCOL_CGM_MODE_GAMMA) && cs->gamma_lut)
			chv_load_cgm_gamma(d, pipe, cs->gamma_lut);
		else
			lg_gamma_load(d, pipe, cs->gamma);
		/* single buffered: last, so the tables are in place */
		lg_wr(d, LCOL_CGM_MODE(pipe), s->cgm_mode);
		return;
	}
	if (d->gmch) {
		if (s->wgc_enable)
			vlv_load_wgc_csc(d, pipe, s);
		if (s->gamma_mode == LCOL_GAMMA_MODE_10BIT && cs->gamma_lut) {
			if (d->ver < 4)
				i9xx_load_lut_10(d, pipe, cs->gamma_lut);
			else
				i965_load_lut_10p6(d, pipe, cs->gamma_lut);
		} else {
			lg_gamma_load(d, pipe, cs->gamma);
		}
		return;
	}
	/* Ironlake to Ivy Bridge */
	if (!s->legacy) {
		if (s->csc_enable)
			ilk_load_csc(d, pipe, s);
		lg_wr(d, LCOL_CSC_MODE(pipe), s->csc_mode);
	}
	switch (s->gamma_mode) {
	case LCOL_GAMMA_MODE_SPLIT:
		ivb_load_lut_10(d, pipe, cs->degamma_lut, 512,
				LCOL_PAL_PREC_SPLIT_MODE | LCOL_PAL_PREC_INDEX_VALUE(0));
		ivb_load_lut_ext_max(d, pipe);
		ivb_load_lut_10(d, pipe, cs->gamma_lut, 512,
				LCOL_PAL_PREC_SPLIT_MODE | LCOL_PAL_PREC_INDEX_VALUE(512));
		break;
	case LCOL_GAMMA_MODE_10BIT: {
		const struct drm_blob *blob = lut_of(cs, s->post_lut);
		if (!blob)
			blob = lut_of(cs, s->pre_lut);
		if (!blob)
			break;
		if (d->is_ivb) {
			ivb_load_lut_10(d, pipe, blob, lut_len(blob), LCOL_PAL_PREC_INDEX_VALUE(0));
			ivb_load_lut_ext_max(d, pipe);
		} else {
			ilk_load_lut_10(d, pipe, blob);
		}
		break;
	}
	default:
		lg_gamma_load(d, pipe, cs->gamma);
		break;
	}
}

/* ---- the entry points ------------------------------------------------------------ */

void lg_color_prepare(struct lg_display *d, int pipe, const struct drm_crtc_state *cs,
		      struct lg_config *cfg)
{
	if (pipe < 0 || pipe >= LG_MAX_PIPES)
		return;
	compute_state(d, cs, &g_state[pipe]);
	cfg->gamma_mode = LCOL_PIPECONF_GAMMA_MODE(g_state[pipe].gamma_mode);
}

uint32_t lg_color_pipeconf_bits(struct lg_display *d, int pipe)
{
	(void)d;
	if (pipe < 0 || pipe >= LG_MAX_PIPES)
		return 0;
	return g_state[pipe].wgc_enable ? PIPECONF_WGC_ENABLE : 0;
}

void lg_color_load(struct lg_display *d, int pipe, const struct drm_crtc_state *cs)
{
	if (pipe < 0 || pipe >= LG_MAX_PIPES)
		return;
	load_state(d, pipe, &g_state[pipe], cs);
}

int lg_color_update(struct lg_display *d, int pipe, const struct drm_crtc_state *cs)
{
	struct lcol_state *s;
	uint32_t planes_before, conf, mask;

	if (pipe < 0 || pipe >= LG_MAX_PIPES)
		return 0;
	s = &g_state[pipe];
	planes_before = lg_color_plane_bits(d, pipe, 0) | lg_color_plane_bits(d, pipe, 1) << 1;
	compute_state(d, cs, s);
	/* the matrix before the configuration that puts it in the path */
	if (d->is_vlv && s->wgc_enable)
		vlv_load_wgc_csc(d, pipe, s);
	if (!d->gmch && !s->legacy && s->csc_enable)
		ilk_load_csc(d, pipe, s);
	/* the gamma mode (and the WGC enable) of PIPECONF */
	mask = d->gmch ? PIPECONF_GAMMA_MODE_MASK_I9XX : PIPECONF_GAMMA_MODE_MASK_ILK;
	if (d->is_vlv)
		mask |= PIPECONF_WGC_ENABLE;
	conf = lg_rd(d, PIPECONF(d, pipe));
	conf = (conf & ~mask) | LCOL_PIPECONF_GAMMA_MODE(s->gamma_mode) |
	       lg_color_pipeconf_bits(d, pipe);
	lg_wr(d, PIPECONF(d, pipe), conf);
	lg_posting_read(d, PIPECONF(d, pipe));
	d->pipes[pipe].cfg.gamma_mode = LCOL_PIPECONF_GAMMA_MODE(s->gamma_mode);
	if (d->pipes[pipe].output >= 0 && d->pipes[pipe].output < d->nout)
		d->out[d->pipes[pipe].output].cfg.gamma_mode = d->pipes[pipe].cfg.gamma_mode;
	load_state(d, pipe, s, cs);
	return planes_before !=
	       (lg_color_plane_bits(d, pipe, 0) | lg_color_plane_bits(d, pipe, 1) << 1);
}

uint32_t lg_color_plane_bits(struct lg_display *d, int pipe, int cursor)
{
	const struct lcol_state *s;
	uint32_t bits = 0;

	if (pipe < 0 || pipe >= LG_MAX_PIPES)
		return 0;
	s = &g_state[pipe];
	if (!cursor) {
		if (s->gamma_enable)
			bits |= DISP_PIPE_GAMMA_ENABLE;
		if (s->csc_enable && !d->gmch)
			bits |= DISP_PIPE_CSC_ENABLE;
		return bits;
	}
	if (d->is_i845 || d->is_i865)
		return s->gamma_enable ? CURSOR_PIPE_GAMMA_ENABLE : 0;
	if (s->gamma_enable)
		bits |= MCURSOR_PIPE_GAMMA_ENABLE;
	if (s->csc_enable && !d->gmch)
		bits |= MCURSOR_PIPE_CSC_ENABLE;
	return bits;
}

int lg_color_crtc_init(struct lg_display *d, int crtc)
{
	struct drm_device *dev = d->drm;
	struct lcol_caps caps;
	int rc;

	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return -EINVAL;
	caps_for(d->platform, &caps);
	/* SETGAMMA / GETGAMMA keep the 256-entry table on every part */
	rc = drm_mode_crtc_set_gamma_size(&dev->crtc[crtc], LCOL_LEGACY_LUT_LENGTH);
	if (rc)
		return rc;
	return drm_crtc_enable_color_mgmt(dev, &dev->crtc[crtc], caps.degamma_size,
					  caps.has_ctm != 0, caps.gamma_size);
}

void lg_color_driver_setup(const struct i915_device *i915, struct drm_driver *drv)
{
#if I915_FEAT_LEGACY_COLOR_MGMT
	struct lcol_caps caps;

	caps_for(i915->info->platform, &caps);
	/* The legacy table stays the device's default size (256): each
	 * crtc's GAMMA_LUT_SIZE is set in lg_color_crtc_init(). */
	drv->degamma_size = caps.degamma_size;
	drv->color_has_ctm = caps.has_ctm;
	if (caps.degamma_size || caps.has_ctm)
		drv->features |= DRM_FEATURE_COLOR_MGMT;
#else
	(void)i915;
	(void)drv;
#endif
}
