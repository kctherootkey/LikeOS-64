// LikeOS -- colour management of the Intel display pipes (the DDI
// displays: Haswell on).
//
// Each pipe runs its picture through its colour stages before the
// transcoder.  Without a client's colour tables that is the 8-bit legacy
// palette (LGC_PALETTE) as the pipe gamma, loaded from the crtc state's
// 256-entry table -- what the pipes have always run.  A client that sets
// DEGAMMA_LUT, CTM or GAMMA_LUT gets the pipe's colour hardware as each
// platform has it:
//
//   Haswell, Broadwell, display 9   one 1024-entry precision palette that
//                                   is the degamma or the gamma table
//                                   (10-bit), or both halves of it (split
//                                   mode, 512 entries each); the CSC
//                                   before or after it (CSC_MODE)
//   display 10 (Gemini Lake)        a 33-entry degamma table (one value
//                                   for all three channels) always ahead
//                                   of the CSC, a 1024-entry 10-bit gamma
//   display 11, 12                  degamma 33; gamma multi-segmented
//                                   (262145 entries: 9 super-fine, 256
//                                   fine, 256 coarse and the maximum) or
//                                   1024 10-bit; GAMMA_MODE enables each
//                                   stage and CSC_MODE the CSC
//   display 13 on                   degamma 129 (24-bit entries from 14);
//                                   gamma 1024 10-bit
//
// A 256-entry GAMMA_LUT is the legacy table on every platform: the 8-bit
// palette.  The CTM goes into the pipe CSC in its coefficient format (a
// sign, a 3-bit exponent and a 9-bit mantissa per coefficient).
//
// Programming is plain MMIO.  The double-buffered registers (the CSC
// coefficients, then GAMMA_MODE / CSC_MODE / the bottom colour and the
// planes' enable bits) are written so that they take effect together at
// the next vblank; the tables, which are not double-buffered before
// display 30, are written once that vblank has latched the new mode, at
// the start of the blanking period -- or first, while the hardware does
// not read any table.  A full mode set writes everything before the pipe
// shows anything.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2016 Intel Corporation
// Portions Copyright (C) 2023 Intel Corporation
// Portions Copyright (C) 2020 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_color.h>
#include <kernel/dev/gpu/i915/intel_color_regs.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

/* ---- the gamma table ------------------------------------------------------------ */

/* The pipe's 8-bit palette: 256 entries of 8:8:8, the high byte of each
 * 16-bit value the client handed over. */
static void pipe_gamma_palette(struct i915_device *i915, int pipe, const uint16_t gamma[3][256])
{
	for (int i = 0; i < 256; i++)
		i915_write32(i915, LGC_PALETTE(pipe, i),
			     ((uint32_t)(gamma[0][i] >> 8) << 16) |
				     ((uint32_t)(gamma[1][i] >> 8) << 8) |
				     (uint32_t)(gamma[2][i] >> 8));
}

static void pipe_gamma_program(struct i915_device *i915, struct intel_pipe *p,
			       const uint16_t gamma[3][256])
{
	i915_write32(i915, GAMMA_MODE(p->pipe), GAMMA_MODE_MODE_8BIT);
	pipe_gamma_palette(i915, p->pipe, gamma);
}

/* ---- what each platform's pipes have ------------------------------------------ */

enum color_kind {
	COLOR_KIND_NONE = 0, /* the older displays: not handled here */
	COLOR_KIND_HSW, /* Haswell: index written per precision palette entry */
	COLOR_KIND_BDW, /* Broadwell: the index auto-increments */
	COLOR_KIND_SKL, /* display 9: plus the pipe bottom colour */
	COLOR_KIND_GLK, /* display 10: the degamma table ahead of the CSC */
	COLOR_KIND_ICL, /* display 11: GAMMA_MODE / CSC_MODE enables */
	COLOR_KIND_TGL, /* display 12 on */
};

#define LEGACY_LUT_LENGTH 256
#define ICL_MULTISEG_LUT_LENGTH 262145

static enum color_kind color_kind(const struct i915_device *i915)
{
	int ver = i915->info->display_ver;

	if (intel_display_legacy_part(i915))
		return COLOR_KIND_NONE;
	if (i915->info->platform == I915_PLATFORM_HASWELL)
		return COLOR_KIND_HSW;
	if (ver >= 12)
		return COLOR_KIND_TGL;
	if (ver == 11)
		return COLOR_KIND_ICL;
	if (ver == 10)
		return COLOR_KIND_GLK;
	if (ver == 9)
		return COLOR_KIND_SKL;
	if (ver == 8)
		return COLOR_KIND_BDW;
	return COLOR_KIND_NONE;
}

/* The client's colour tables are taken on this part. */
static int color_mgmt_on(const struct i915_device *i915)
{
	return I915_FEAT_COLOR_MGMT && color_kind(i915) != COLOR_KIND_NONE;
}

/* Display 11 and 12 offer the multi-segmented gamma table: a 2 MiB blob,
 * so the core must take blobs that large, and GAMMA_LUT_SIZE must be a
 * size of its own (not the legacy table's). */
static int color_multiseg(const struct i915_device *i915)
{
	int ver = i915->info->display_ver;

	if (!I915_FEAT_COLOR_ICL_MULTISEG || ver < 11 || ver > 12)
		return 0;
	if ((uint64_t)DRM_PROPERTY_BLOB_MAX_LENGTH <
	    (uint64_t)ICL_MULTISEG_LUT_LENGTH * sizeof(struct drm_color_lut))
		return 0;
	return DRM_COLOR_LEGACY_GAMMA_SPLIT;
}

/* GAMMA_LUT_SIZE: the table the pipe's gamma takes besides the legacy
 * 256 entries.  Where the core has one size for both, only those. */
static uint32_t color_gamma_lut_size(const struct i915_device *i915)
{
	if (!DRM_COLOR_LEGACY_GAMMA_SPLIT)
		return LEGACY_LUT_LENGTH;
	if (color_multiseg(i915))
		return ICL_MULTISEG_LUT_LENGTH;
	return 1024;
}

/* DEGAMMA_LUT_SIZE */
static uint32_t color_degamma_lut_size(const struct i915_device *i915)
{
	switch (color_kind(i915)) {
	case COLOR_KIND_HSW:
	case COLOR_KIND_BDW:
	case COLOR_KIND_SKL:
		return 1024;
	case COLOR_KIND_GLK:
	case COLOR_KIND_ICL:
		return 33;
	case COLOR_KIND_TGL:
		return i915->info->display_ver >= 13 ? 129 : 33;
	default:
		return 0;
	}
}

/* The degamma table of display 10 on has one value per entry for all
 * three channels, and it interpolates: it must not fall. */
static uint32_t color_degamma_lut_tests(const struct i915_device *i915)
{
	if (color_kind(i915) >= COLOR_KIND_GLK)
		return DRM_COLOR_LUT_NON_DECREASING | DRM_COLOR_LUT_EQUAL_CHANNELS;
	return 0;
}

static int lut_is_legacy(const struct drm_blob *lut)
{
	return lut && drm_color_lut_size(lut) == LEGACY_LUT_LENGTH;
}

/* The segments of the multi-segmented gamma are interpolated: such a
 * table must not fall.  The legacy table is never checked. */
static uint32_t color_gamma_lut_tests(const struct i915_device *i915, const struct drm_blob *gamma)
{
	if (lut_is_legacy(gamma))
		return 0;
	if (color_multiseg(i915))
		return DRM_COLOR_LUT_NON_DECREASING;
	return 0;
}

/* Entries in the hardware's degamma table: the client's, and two more
 * for inputs beyond 1.0 that are clamped to 1.0. */
static int glk_degamma_lut_size(const struct i915_device *i915)
{
	if (i915->info->display_ver >= 13)
		return 131;
	else
		return 35;
}

/* ---- tables as loaded -------------------------------------------------------- */

/* A table to load: a client's blob, possibly resampled to fewer entries
 * (the halves of the split palette), or a linear ramp. */
struct color_lut {
	const struct drm_color_lut *data;
	uint32_t len; /* entries in data, or in the ramp */
	uint32_t out; /* entries loaded */
	uint8_t present;
	uint8_t linear;
};

static void color_lut_blob(struct color_lut *l, const struct drm_blob *blob, uint32_t out)
{
	mm_memset(l, 0, sizeof(*l));
	if (!blob)
		return;
	l->data = drm_color_lut_data(blob);
	l->len = (uint32_t)drm_color_lut_size(blob);
	l->out = out ? out : l->len;
	l->present = l->len >= 2;
}

static void color_lut_linear(struct color_lut *l, uint32_t len)
{
	mm_memset(l, 0, sizeof(*l));
	l->len = l->out = len;
	l->present = 1;
	l->linear = 1;
}

/* Entry i of what is loaded: resampling picks the entry at the same
 * relative position of the client's table. */
static struct drm_color_lut color_lut_entry(const struct color_lut *l, uint32_t i)
{
	struct drm_color_lut e;

	if (l->linear) {
		uint16_t val = (uint16_t)(0xffffu * i / (l->len - 1));

		e.red = e.green = e.blue = val;
		e.reserved = 0;
		return e;
	}
	if (l->out != l->len)
		i = (uint32_t)((uint64_t)i * (l->len - 1) / (l->out - 1));
	return l->data[i];
}

/* ---- the register formats of the entries ---------------------------------------- */

/* 8:8:8 of the legacy palette */
static u32 i9xx_lut_8(const struct drm_color_lut *color)
{
	return (u32)(FIELD_PREP(PALETTE_RED_MASK, drm_color_lut_extract(color->red, 8)) |
		     FIELD_PREP(PALETTE_GREEN_MASK, drm_color_lut_extract(color->green, 8)) |
		     FIELD_PREP(PALETTE_BLUE_MASK, drm_color_lut_extract(color->blue, 8)));
}

/* 10:10:10 of the precision palette */
static u32 ilk_lut_10(const struct drm_color_lut *color)
{
	return (u32)(FIELD_PREP(PREC_PALETTE_10_RED_MASK, drm_color_lut_extract(color->red, 10)) |
		     FIELD_PREP(PREC_PALETTE_10_GREEN_MASK, drm_color_lut_extract(color->green, 10)) |
		     FIELD_PREP(PREC_PALETTE_10_BLUE_MASK, drm_color_lut_extract(color->blue, 10)));
}

/* "12.4" interpolated format: the low 6 bits of each channel... */
static u32 ilk_lut_12p4_ldw(const struct drm_color_lut *color)
{
	return (u32)(FIELD_PREP(PREC_PALETTE_12P4_RED_LDW_MASK, color->red & 0x3f) |
		     FIELD_PREP(PREC_PALETTE_12P4_GREEN_LDW_MASK, color->green & 0x3f) |
		     FIELD_PREP(PREC_PALETTE_12P4_BLUE_LDW_MASK, color->blue & 0x3f));
}

/* ...and the high 10 bits */
static u32 ilk_lut_12p4_udw(const struct drm_color_lut *color)
{
	return (u32)(FIELD_PREP(PREC_PALETTE_12P4_RED_UDW_MASK, color->red >> 6) |
		     FIELD_PREP(PREC_PALETTE_12P4_GREEN_UDW_MASK, color->green >> 6) |
		     FIELD_PREP(PREC_PALETTE_12P4_BLUE_UDW_MASK, color->blue >> 6));
}

/* The degamma table holds one 0.16 value (3.16 register) per entry: the
 * channels are equal, green stands for all three. */
static u32 glk_degamma_lut(const struct drm_color_lut *color)
{
	return color->green;
}

/* Display 14 on: 0.24 values (3.24 register). */
static u32 mtl_degamma_lut(const struct drm_color_lut *color)
{
	return drm_color_lut_extract(color->green, 24);
}

/* ---- the CSC ------------------------------------------------------------------- */

/*
 * The pipe CSC:
 *
 * |R/Cr|   | c0 c1 c2 |   ( |R/Cr|   |preoff0| )   |postoff0|
 * |G/Y | = | c3 c4 c5 | x ( |G/Y | + |preoff1| ) + |postoff1|
 * |B/Cb|   | c6 c7 c8 |   ( |B/Cb|   |preoff2| )   |postoff2|
 *
 * Each coefficient is 16 bits: the sign (bit 15), an exponent (bits 14:12)
 * and a 9-bit mantissa (bits 11:3); the exponent selects where the 9 bits
 * sit -- 0.125 steps from 2^-12 up to 4.0.
 */
struct intel_csc_matrix {
	u16 coeff[9];
	u16 preoff[3];
	u16 postoff[3];
};

#define CTM_COEFF_SIGN (1ULL << 63)

#define CTM_COEFF_1_0 (1ULL << 32)
#define CTM_COEFF_2_0 (CTM_COEFF_1_0 << 1)
#define CTM_COEFF_4_0 (CTM_COEFF_2_0 << 1)
#define CTM_COEFF_8_0 (CTM_COEFF_4_0 << 1)
#define CTM_COEFF_0_5 (CTM_COEFF_1_0 >> 1)
#define CTM_COEFF_0_25 (CTM_COEFF_0_5 >> 1)
#define CTM_COEFF_0_125 (CTM_COEFF_0_25 >> 1)

#define CTM_COEFF_LIMITED_RANGE ((235ULL - 16ULL) * CTM_COEFF_1_0 / 255)

#define CTM_COEFF_NEGATIVE(coeff) (((coeff) & CTM_COEFF_SIGN) != 0)
#define CTM_COEFF_ABS(coeff) ((coeff) & (CTM_COEFF_SIGN - 1))

/*
 * The coefficient from a CTM coefficient (S31.32 sign-magnitude) and the
 * number of fraction bits the exponent leaves: a 9-bit window that slides
 * with the coefficient's size, placed at bit 3, rounded.
 */
#define ILK_CSC_COEFF_FP(coeff, fbits) \
	(clamp_val(((coeff) >> (32 - (fbits) - 3)) + 4, 0, 0xfff) & 0xff8)

#define ILK_CSC_COEFF_1_0 0x7800
#define ILK_CSC_COEFF_LIMITED_RANGE ((235 - 16) << (12 - 8)) /* exponent 0 */
#define ILK_CSC_POSTOFF_LIMITED_RANGE (16 << (12 - 8))

static const struct intel_csc_matrix ilk_csc_matrix_identity = {
	.preoff = { 0, 0, 0 },
	.coeff = {
		ILK_CSC_COEFF_1_0, 0, 0,
		0, ILK_CSC_COEFF_1_0, 0,
		0, 0, ILK_CSC_COEFF_1_0,
	},
	.postoff = { 0, 0, 0 },
};

/* Full range RGB -> limited range RGB */
static const struct intel_csc_matrix ilk_csc_matrix_limited_range = {
	.preoff = { 0, 0, 0 },
	.coeff = {
		ILK_CSC_COEFF_LIMITED_RANGE, 0, 0,
		0, ILK_CSC_COEFF_LIMITED_RANGE, 0,
		0, 0, ILK_CSC_COEFF_LIMITED_RANGE,
	},
	.postoff = {
		ILK_CSC_POSTOFF_LIMITED_RANGE,
		ILK_CSC_POSTOFF_LIMITED_RANGE,
		ILK_CSC_POSTOFF_LIMITED_RANGE,
	},
};

/* For a limited-range output the client's matrix is scaled by the
 * matrix that compresses the range (16-235 of 0-255), so the result fits
 * what the sink takes. */
static u64 *ctm_mult_by_limited(u64 *result, const u64 *input)
{
	for (int i = 0; i < 9; i++) {
		u64 user_coeff = input[i];
		u32 limited_coeff = (u32)CTM_COEFF_LIMITED_RANGE;
		u32 abs_coeff = (u32)(clamp_val(CTM_COEFF_ABS(user_coeff), 0ULL,
						CTM_COEFF_4_0 - 1) >> 2);

		result[i] = ((u64)limited_coeff * abs_coeff) >> 30;
		result[i] |= user_coeff & CTM_COEFF_SIGN;
	}
	return result;
}

/* The CTM in the pipe CSC's format (with the offsets of a limited-range
 * output when `limited_color_range'). */
static void ilk_csc_convert_ctm(const struct drm_color_ctm *ctm, struct intel_csc_matrix *csc,
				int limited_color_range)
{
	const u64 *input;
	u64 temp[9];

	/* for preoff/postoff */
	if (limited_color_range)
		*csc = ilk_csc_matrix_limited_range;
	else
		*csc = ilk_csc_matrix_identity;

	if (limited_color_range)
		input = ctm_mult_by_limited(temp, ctm->matrix);
	else
		input = ctm->matrix;

	for (int i = 0; i < 9; i++) {
		u64 abs_coeff = ((1ULL << 63) - 1) & input[i];

		/* what the hardware can hold */
		abs_coeff = clamp_val(abs_coeff, 0ULL, CTM_COEFF_4_0 - 1);

		csc->coeff[i] = 0;

		/* sign bit */
		if (CTM_COEFF_NEGATIVE(input[i]))
			csc->coeff[i] |= 1 << 15;

		if (abs_coeff < CTM_COEFF_0_125)
			csc->coeff[i] |= (u16)((3 << 12) | ILK_CSC_COEFF_FP(abs_coeff, 12));
		else if (abs_coeff < CTM_COEFF_0_25)
			csc->coeff[i] |= (u16)((2 << 12) | ILK_CSC_COEFF_FP(abs_coeff, 11));
		else if (abs_coeff < CTM_COEFF_0_5)
			csc->coeff[i] |= (u16)((1 << 12) | ILK_CSC_COEFF_FP(abs_coeff, 10));
		else if (abs_coeff < CTM_COEFF_1_0)
			csc->coeff[i] |= (u16)ILK_CSC_COEFF_FP(abs_coeff, 9);
		else if (abs_coeff < CTM_COEFF_2_0)
			csc->coeff[i] |= (u16)((7 << 12) | ILK_CSC_COEFF_FP(abs_coeff, 8));
		else
			csc->coeff[i] |= (u16)((6 << 12) | ILK_CSC_COEFF_FP(abs_coeff, 7));
	}
}

static void ilk_update_pipe_csc(struct i915_device *i915, int pipe,
				const struct intel_csc_matrix *csc)
{
	i915_write32(i915, PIPE_CSC_PREOFF_HI(pipe), csc->preoff[0]);
	i915_write32(i915, PIPE_CSC_PREOFF_ME(pipe), csc->preoff[1]);
	i915_write32(i915, PIPE_CSC_PREOFF_LO(pipe), csc->preoff[2]);

	i915_write32(i915, PIPE_CSC_COEFF_RY_GY(pipe),
		     (u32)csc->coeff[0] << 16 | csc->coeff[1]);
	i915_write32(i915, PIPE_CSC_COEFF_BY(pipe), (u32)csc->coeff[2] << 16);

	i915_write32(i915, PIPE_CSC_COEFF_RU_GU(pipe),
		     (u32)csc->coeff[3] << 16 | csc->coeff[4]);
	i915_write32(i915, PIPE_CSC_COEFF_BU(pipe), (u32)csc->coeff[5] << 16);

	i915_write32(i915, PIPE_CSC_COEFF_RV_GV(pipe),
		     (u32)csc->coeff[6] << 16 | csc->coeff[7]);
	i915_write32(i915, PIPE_CSC_COEFF_BV(pipe), (u32)csc->coeff[8] << 16);

	/* every display handled here has the post offsets (version 7 on) */
	i915_write32(i915, PIPE_CSC_POSTOFF_HI(pipe), csc->postoff[0]);
	i915_write32(i915, PIPE_CSC_POSTOFF_ME(pipe), csc->postoff[1]);
	i915_write32(i915, PIPE_CSC_POSTOFF_LO(pipe), csc->postoff[2]);
}

static void icl_update_output_csc(struct i915_device *i915, int pipe,
				  const struct intel_csc_matrix *csc)
{
	i915_write32(i915, PIPE_CSC_OUTPUT_PREOFF_HI(pipe), csc->preoff[0]);
	i915_write32(i915, PIPE_CSC_OUTPUT_PREOFF_ME(pipe), csc->preoff[1]);
	i915_write32(i915, PIPE_CSC_OUTPUT_PREOFF_LO(pipe), csc->preoff[2]);

	i915_write32(i915, PIPE_CSC_OUTPUT_COEFF_RY_GY(pipe),
		     (u32)csc->coeff[0] << 16 | csc->coeff[1]);
	i915_write32(i915, PIPE_CSC_OUTPUT_COEFF_BY(pipe), (u32)csc->coeff[2] << 16);

	i915_write32(i915, PIPE_CSC_OUTPUT_COEFF_RU_GU(pipe),
		     (u32)csc->coeff[3] << 16 | csc->coeff[4]);
	i915_write32(i915, PIPE_CSC_OUTPUT_COEFF_BU(pipe), (u32)csc->coeff[5] << 16);

	i915_write32(i915, PIPE_CSC_OUTPUT_COEFF_RV_GV(pipe),
		     (u32)csc->coeff[6] << 16 | csc->coeff[7]);
	i915_write32(i915, PIPE_CSC_OUTPUT_COEFF_BV(pipe), (u32)csc->coeff[8] << 16);

	i915_write32(i915, PIPE_CSC_OUTPUT_POSTOFF_HI(pipe), csc->postoff[0]);
	i915_write32(i915, PIPE_CSC_OUTPUT_POSTOFF_ME(pipe), csc->postoff[1]);
	i915_write32(i915, PIPE_CSC_OUTPUT_POSTOFF_LO(pipe), csc->postoff[2]);
}

/* ---- the configuration a crtc state asks for ---------------------------------- */

struct color_cfg {
	uint8_t managed; /* 0: the 8-bit palette path */
	uint8_t gamma_enable; /* the pipe gamma, as the planes enable it */
	uint8_t csc_enable; /* the pipe CSC, likewise */
	uint32_t gamma_mode; /* GAMMA_MODE */
	uint32_t csc_mode; /* CSC_MODE */
	struct color_lut pre; /* the table ahead of the CSC */
	struct color_lut post; /* the table after it */
	struct intel_csc_matrix csc;
	struct intel_csc_matrix output_csc;
};

/* The outputs are driven as full-range RGB: neither a limited-range nor a
 * YCbCr output exists yet, so the pipe never compresses the range and
 * the output CSC stays off. */
static int color_limited_range(void)
{
	return 0;
}

/* Haswell, Broadwell, display 9 */
static void ivb_color_compute(const struct drm_crtc_state *cs, struct color_cfg *c)
{
	const struct drm_blob *degamma = cs->degamma_lut, *gamma = cs->gamma_lut;

	c->gamma_enable = gamma || degamma;
	c->csc_enable = cs->ctm != NULL;

	if (degamma && gamma)
		c->gamma_mode = GAMMA_MODE_MODE_SPLIT;
	else if (!c->gamma_enable || lut_is_legacy(gamma))
		c->gamma_mode = GAMMA_MODE_MODE_8BIT;
	else
		c->gamma_mode = GAMMA_MODE_MODE_10BIT;

	/* the CSC comes after the table when that is the degamma */
	c->csc_mode = degamma ? 0 : CSC_POSITION_BEFORE_GAMMA;

	if (c->gamma_mode == GAMMA_MODE_MODE_SPLIT) {
		/* each half of the palette takes 512 entries */
		color_lut_blob(&c->pre, degamma, 512);
		color_lut_blob(&c->post, gamma, 512);
	} else {
		color_lut_blob(&c->pre, degamma, 0);
		color_lut_blob(&c->post, gamma, 0);
	}

	if (cs->ctm)
		ilk_csc_convert_ctm(cs->ctm->data, &c->csc, color_limited_range());
}

/* display 10 */
static void glk_color_compute(const struct drm_crtc_state *cs, struct color_cfg *c)
{
	const struct drm_blob *degamma = cs->degamma_lut, *gamma = cs->gamma_lut;

	c->gamma_enable = gamma != NULL;
	/* the degamma table is enabled together with the CSC */
	c->csc_enable = degamma || cs->ctm;

	if (!c->gamma_enable || lut_is_legacy(gamma))
		c->gamma_mode = GAMMA_MODE_MODE_8BIT;
	else
		c->gamma_mode = GAMMA_MODE_MODE_10BIT;
	c->csc_mode = 0;

	color_lut_blob(&c->post, gamma, 0);
	color_lut_blob(&c->pre, degamma, 0);
	/* the CSC without a degamma table: a linear one */
	if (c->csc_enable && !c->pre.present)
		color_lut_linear(&c->pre, 33);

	if (cs->ctm)
		ilk_csc_convert_ctm(cs->ctm->data, &c->csc, color_limited_range());
	else if (c->csc_enable)
		c->csc = ilk_csc_matrix_identity;
}

/* display 11 on */
static void icl_color_compute(const struct i915_device *i915, const struct drm_crtc_state *cs,
			      struct color_cfg *c)
{
	const struct drm_blob *degamma = cs->degamma_lut, *gamma = cs->gamma_lut;

	c->gamma_mode = 0;
	if (degamma)
		c->gamma_mode |= PRE_CSC_GAMMA_ENABLE;
	if (gamma)
		c->gamma_mode |= POST_CSC_GAMMA_ENABLE;
	if (!gamma || lut_is_legacy(gamma))
		c->gamma_mode |= GAMMA_MODE_MODE_8BIT;
	else if (i915->info->display_ver >= 13 || !color_multiseg(i915))
		c->gamma_mode |= GAMMA_MODE_MODE_10BIT;
	else
		c->gamma_mode |= GAMMA_MODE_MODE_12BIT_MULTI_SEG;

	c->csc_mode = 0;
	if (cs->ctm)
		c->csc_mode |= ICL_CSC_ENABLE;
	if (color_limited_range())
		c->csc_mode |= ICL_OUTPUT_CSC_ENABLE;

	color_lut_blob(&c->pre, degamma, 0);
	color_lut_blob(&c->post, gamma, 0);

	if (cs->ctm)
		ilk_csc_convert_ctm(cs->ctm->data, &c->csc, 0);
	if (color_limited_range())
		c->output_csc = ilk_csc_matrix_limited_range;
}

/* What the pipe is to run for crtc state `cs'.  Without a degamma table,
 * gamma table or matrix (or with the feature off) that is the palette
 * path (c->managed 0).  0, or -EINVAL with *why when a table does not
 * have a size the pipe takes. */
static int color_compute(const struct i915_device *i915, const struct drm_crtc_state *cs,
			 struct color_cfg *c, const char **why)
{
	const struct drm_blob *degamma = cs->degamma_lut, *gamma = cs->gamma_lut;
	enum color_kind kind = color_kind(i915);

	mm_memset(c, 0, sizeof(*c));
	if (!color_mgmt_on(i915) || (!degamma && !gamma && !cs->ctm))
		return 0;

	if (degamma && (uint32_t)drm_color_lut_size(degamma) != color_degamma_lut_size(i915)) {
		*why = "the degamma table does not have the size the pipe takes";
		return -EINVAL;
	}
	if (gamma && !lut_is_legacy(gamma) &&
	    (uint32_t)drm_color_lut_size(gamma) != color_gamma_lut_size(i915)) {
		*why = "the gamma table does not have the size the pipe takes";
		return -EINVAL;
	}
	if (cs->ctm && cs->ctm->length != sizeof(struct drm_color_ctm)) {
		*why = "the colour matrix is not one struct drm_color_ctm";
		return -EINVAL;
	}

	c->managed = 1;
	switch (kind) {
	case COLOR_KIND_HSW:
	case COLOR_KIND_BDW:
	case COLOR_KIND_SKL:
		ivb_color_compute(cs, c);
		break;
	case COLOR_KIND_GLK:
		glk_color_compute(cs, c);
		break;
	default:
		icl_color_compute(i915, cs, c);
		break;
	}
	return 0;
}

/* ---- loading the tables ----------------------------------------------------------- */

static void ilk_load_lut_8(struct i915_device *i915, int pipe, const struct color_lut *l)
{
	if (!l->present)
		return;
	for (uint32_t i = 0; i < LEGACY_LUT_LENGTH && i < l->out; i++) {
		struct drm_color_lut e = color_lut_entry(l, i);

		i915_write32(i915, LGC_PALETTE(pipe, i), i9xx_lut_8(&e));
	}
}

/*
 * Haswell: the index does not auto-increment ("Index auto increment mode
 * is not supported and must not be enabled"), so it is written for every
 * entry.
 */
static void ivb_load_lut_10(struct i915_device *i915, int pipe, const struct color_lut *l,
			    u32 prec_index)
{
	for (uint32_t i = 0; i < l->out; i++) {
		struct drm_color_lut e = color_lut_entry(l, i);

		i915_write32(i915, PREC_PAL_INDEX(pipe), prec_index + i);
		i915_write32(i915, PREC_PAL_DATA(pipe), ilk_lut_10(&e));
	}

	/* An index left set keeps the legacy palette from being written
	 * properly. */
	i915_write32(i915, PREC_PAL_INDEX(pipe), PAL_PREC_INDEX_VALUE(0));
}

/* Broadwell on: the index auto-increments. */
static void bdw_load_lut_10(struct i915_device *i915, int pipe, const struct color_lut *l,
			    u32 prec_index)
{
	i915_write32(i915, PREC_PAL_INDEX(pipe), prec_index);
	i915_write32(i915, PREC_PAL_INDEX(pipe), PAL_PREC_AUTO_INCREMENT | prec_index);

	for (uint32_t i = 0; i < l->out; i++) {
		struct drm_color_lut e = color_lut_entry(l, i);

		i915_write32(i915, PREC_PAL_DATA(pipe), ilk_lut_10(&e));
	}

	i915_write32(i915, PREC_PAL_INDEX(pipe), PAL_PREC_INDEX_VALUE(0));
}

/* Values beyond 1.0 are clamped to 1.0. */
static void ivb_load_lut_ext_max(struct i915_device *i915, int pipe)
{
	i915_write32(i915, PREC_PAL_EXT_GC_MAX(pipe, 0), 1 << 16);
	i915_write32(i915, PREC_PAL_EXT_GC_MAX(pipe, 1), 1 << 16);
	i915_write32(i915, PREC_PAL_EXT_GC_MAX(pipe, 2), 1 << 16);
}

static void glk_load_lut_ext2_max(struct i915_device *i915, int pipe)
{
	i915_write32(i915, PREC_PAL_EXT2_GC_MAX(pipe, 0), 1 << 16);
	i915_write32(i915, PREC_PAL_EXT2_GC_MAX(pipe, 1), 1 << 16);
	i915_write32(i915, PREC_PAL_EXT2_GC_MAX(pipe, 2), 1 << 16);
}

static void ivb_load_luts(struct i915_device *i915, int pipe, const struct color_cfg *c,
			  int auto_increment)
{
	const struct color_lut *blob = c->post.present ? &c->post : &c->pre;
	void (*load_10)(struct i915_device *, int, const struct color_lut *, u32) =
		auto_increment ? bdw_load_lut_10 : ivb_load_lut_10;

	switch (c->gamma_mode) {
	case GAMMA_MODE_MODE_8BIT:
		ilk_load_lut_8(i915, pipe, blob);
		break;
	case GAMMA_MODE_MODE_SPLIT:
		load_10(i915, pipe, &c->pre, PAL_PREC_SPLIT_MODE | PAL_PREC_INDEX_VALUE(0));
		ivb_load_lut_ext_max(i915, pipe);
		load_10(i915, pipe, &c->post, PAL_PREC_SPLIT_MODE | PAL_PREC_INDEX_VALUE(512));
		break;
	case GAMMA_MODE_MODE_10BIT:
		load_10(i915, pipe, blob, PAL_PREC_INDEX_VALUE(0));
		ivb_load_lut_ext_max(i915, pipe);
		break;
	default:
		break;
	}
}

static void glk_load_degamma_lut(struct i915_device *i915, int pipe, const struct color_lut *l)
{
	int mtl = i915->info->display_ver >= 14;
	uint32_t i;

	/* With the auto-increment bit set the hardware seems to ignore the
	 * index bits: index 0 is set on its own first. */
	i915_write32(i915, PRE_CSC_GAMC_INDEX(pipe), PRE_CSC_GAMC_INDEX_VALUE(0));
	i915_write32(i915, PRE_CSC_GAMC_INDEX(pipe),
		     PRE_CSC_GAMC_AUTO_INCREMENT | PRE_CSC_GAMC_INDEX_VALUE(0));

	/*
	 * The client's entries cover 0 to 1.0; the extra hardware entries
	 * after them stand for the extended range up to 3.0 and 7.0 and are
	 * clamped at 1.0.  The table takes one value per entry for all three
	 * channels (the check made the client's channels equal).
	 */
	for (i = 0; i < l->out; i++) {
		struct drm_color_lut e = color_lut_entry(l, i);

		i915_write32(i915, PRE_CSC_GAMC_DATA(pipe),
			     mtl ? mtl_degamma_lut(&e) : glk_degamma_lut(&e));
	}

	/* clamp values > 1.0 */
	while (i++ < (uint32_t)glk_degamma_lut_size(i915))
		i915_write32(i915, PRE_CSC_GAMC_DATA(pipe), mtl ? 1u << 24 : 1u << 16);

	i915_write32(i915, PRE_CSC_GAMC_INDEX(pipe), 0);
}

static void glk_load_luts(struct i915_device *i915, int pipe, const struct color_cfg *c)
{
	if (c->pre.present)
		glk_load_degamma_lut(i915, pipe, &c->pre);

	switch (c->gamma_mode) {
	case GAMMA_MODE_MODE_8BIT:
		ilk_load_lut_8(i915, pipe, &c->post);
		break;
	case GAMMA_MODE_MODE_10BIT:
		bdw_load_lut_10(i915, pipe, &c->post, PAL_PREC_INDEX_VALUE(0));
		ivb_load_lut_ext_max(i915, pipe);
		glk_load_lut_ext2_max(i915, pipe);
		break;
	default:
		break;
	}
}

/* The last entry of the multi-segmented table: 16-bit entries, so 0xffff
 * is the largest maximum that can be programmed. */
static void ivb_load_lut_max(struct i915_device *i915, int pipe, const struct drm_color_lut *color)
{
	i915_write32(i915, PREC_PAL_GC_MAX(pipe, 0), color->red);
	i915_write32(i915, PREC_PAL_GC_MAX(pipe, 1), color->green);
	i915_write32(i915, PREC_PAL_GC_MAX(pipe, 2), color->blue);
}

/*
 * The super-fine segment: steps of 1/(8 * 128 * 256), 9 entries for the
 * values 0, 1/(8 * 128 * 256) ... 8/(8 * 128 * 256).
 */
static void icl_program_gamma_superfine_segment(struct i915_device *i915, int pipe,
						const struct color_lut *l)
{
	i915_write32(i915, PREC_PAL_MULTI_SEG_INDEX(pipe), PAL_PREC_MULTI_SEG_INDEX_VALUE(0));
	i915_write32(i915, PREC_PAL_MULTI_SEG_INDEX(pipe),
		     PAL_PREC_AUTO_INCREMENT | PAL_PREC_MULTI_SEG_INDEX_VALUE(0));

	for (uint32_t i = 0; i < 9; i++) {
		const struct drm_color_lut *entry = &l->data[i];

		i915_write32(i915, PREC_PAL_MULTI_SEG_DATA(pipe), ilk_lut_12p4_ldw(entry));
		i915_write32(i915, PREC_PAL_MULTI_SEG_DATA(pipe), ilk_lut_12p4_udw(entry));
	}

	i915_write32(i915, PREC_PAL_MULTI_SEG_INDEX(pipe), PAL_PREC_MULTI_SEG_INDEX_VALUE(0));
}

static void icl_program_gamma_multi_segment(struct i915_device *i915, int pipe,
					    const struct color_lut *l)
{
	const struct drm_color_lut *lut = l->data;
	const struct drm_color_lut *entry;

	/*
	 * The fine segment: steps of 1/(128 * 256), the values 1/(128 * 256)
	 * ... 256/(128 * 256) -- every 8th entry of the table, 256 of them.
	 * The first two index values both map to the segment's entry 1, its
	 * entry 0 being unused by the hardware.
	 */
	i915_write32(i915, PREC_PAL_INDEX(pipe), PAL_PREC_INDEX_VALUE(0));
	i915_write32(i915, PREC_PAL_INDEX(pipe), PAL_PREC_AUTO_INCREMENT | PAL_PREC_INDEX_VALUE(0));

	for (uint32_t i = 1; i < 257; i++) {
		entry = &lut[i * 8];
		i915_write32(i915, PREC_PAL_DATA(pipe), ilk_lut_12p4_ldw(entry));
		i915_write32(i915, PREC_PAL_DATA(pipe), ilk_lut_12p4_udw(entry));
	}

	/*
	 * The coarse segment: from 0 in steps of 1/256 -- every (8 * 128)th
	 * entry, 256 of them.  Whether its entries 0 and 1 are used is not
	 * clear; they are written to advance the index either way.
	 */
	for (uint32_t i = 0; i < 256; i++) {
		entry = &lut[i * 8 * 128];
		i915_write32(i915, PREC_PAL_DATA(pipe), ilk_lut_12p4_ldw(entry));
		i915_write32(i915, PREC_PAL_DATA(pipe), ilk_lut_12p4_udw(entry));
	}

	i915_write32(i915, PREC_PAL_INDEX(pipe), PAL_PREC_INDEX_VALUE(0));

	/* the table's last entry is the maximum */
	entry = &lut[256 * 8 * 128];
	ivb_load_lut_max(i915, pipe, entry);
}

static void icl_load_luts(struct i915_device *i915, int pipe, const struct color_cfg *c)
{
	if (c->pre.present)
		glk_load_degamma_lut(i915, pipe, &c->pre);

	switch (c->gamma_mode & GAMMA_MODE_MODE_MASK) {
	case GAMMA_MODE_MODE_8BIT:
		ilk_load_lut_8(i915, pipe, &c->post);
		break;
	case GAMMA_MODE_MODE_12BIT_MULTI_SEG:
		if (!c->post.present || c->post.len != ICL_MULTISEG_LUT_LENGTH)
			break;
		icl_program_gamma_superfine_segment(i915, pipe, &c->post);
		icl_program_gamma_multi_segment(i915, pipe, &c->post);
		ivb_load_lut_ext_max(i915, pipe);
		glk_load_lut_ext2_max(i915, pipe);
		break;
	case GAMMA_MODE_MODE_10BIT:
		bdw_load_lut_10(i915, pipe, &c->post, PAL_PREC_INDEX_VALUE(0));
		ivb_load_lut_ext_max(i915, pipe);
		glk_load_lut_ext2_max(i915, pipe);
		break;
	default:
		break;
	}
}

/* The tables of configuration `c' (the 8-bit palette from the crtc's
 * legacy table on the palette path). */
static void color_load_luts(struct i915_device *i915, int pipe, const struct color_cfg *c,
			    const struct drm_crtc_state *cs)
{
	if (!c->managed) {
		pipe_gamma_palette(i915, pipe, cs->gamma);
		return;
	}
	switch (color_kind(i915)) {
	case COLOR_KIND_HSW:
		ivb_load_luts(i915, pipe, c, 0);
		break;
	case COLOR_KIND_BDW:
	case COLOR_KIND_SKL:
		ivb_load_luts(i915, pipe, c, 1);
		break;
	case COLOR_KIND_GLK:
		glk_load_luts(i915, pipe, c);
		break;
	case COLOR_KIND_ICL:
	case COLOR_KIND_TGL:
		icl_load_luts(i915, pipe, c);
		break;
	default:
		break;
	}
}

/* ---- the double-buffered registers ------------------------------------------------ */

/* The CSC coefficients and offsets: they take effect when the mode
 * registers below are written (or the plane arms them). */
static void color_commit_noarm(struct i915_device *i915, int pipe, const struct color_cfg *c)
{
	if (color_kind(i915) >= COLOR_KIND_ICL) {
		if (c->csc_mode & ICL_CSC_ENABLE)
			ilk_update_pipe_csc(i915, pipe, &c->csc);
		if (c->csc_mode & ICL_OUTPUT_CSC_ENABLE)
			icl_update_output_csc(i915, pipe, &c->output_csc);
		return;
	}
	if (c->csc_enable)
		ilk_update_pipe_csc(i915, pipe, &c->csc);
}

static void color_commit_arm(struct i915_device *i915, int pipe, const struct color_cfg *c)
{
	switch (color_kind(i915)) {
	case COLOR_KIND_SKL:
	case COLOR_KIND_GLK: {
		/* black background, through the pipe's gamma and CSC as the
		 * planes are */
		u32 val = 0;

		if (c->gamma_enable)
			val |= SKL_BOTTOM_COLOR_GAMMA_ENABLE;
		if (c->csc_enable)
			val |= SKL_BOTTOM_COLOR_CSC_ENABLE;
		i915_write32(i915, SKL_BOTTOM_COLOR(pipe), val);
		break;
	}
	case COLOR_KIND_ICL:
	case COLOR_KIND_TGL:
		i915_write32(i915, SKL_BOTTOM_COLOR(pipe), 0);
		break;
	default:
		break;
	}
	i915_write32(i915, GAMMA_MODE(pipe), c->gamma_mode);
	i915_write32(i915, PIPE_CSC_MODE(pipe), c->csc_mode);
}

/* Back to the palette path from a client's configuration: the CSC out of
 * the path, the bottom colour through the gamma like the planes, the
 * 8-bit palette mode. */
static void color_commit_arm_palette(struct i915_device *i915, int pipe)
{
	enum color_kind kind = color_kind(i915);

	if (kind == COLOR_KIND_SKL || kind == COLOR_KIND_GLK)
		i915_write32(i915, SKL_BOTTOM_COLOR(pipe), SKL_BOTTOM_COLOR_GAMMA_ENABLE);
	i915_write32(i915, GAMMA_MODE(pipe), GAMMA_MODE_MODE_8BIT);
	i915_write32(i915, PIPE_CSC_MODE(pipe), 0);
}

/*
 * Display 11: once CSC_MODE has armed the CSC it stays armed after the
 * latch, and the coefficient registers become self-arming -- a later
 * write would take effect mid-frame.  A read of a CSC register disarms
 * it again (reads still do); it must come after the previous values were
 * latched.  Display 12 on does not need this.
 */
static void icl_color_csc_disarm(struct i915_device *i915, int pipe)
{
	(void)i915_read32(i915, PIPE_CSC_PREOFF_HI(pipe));
}

/* Until the pipe's next vblank has started: what was written to the
 * double-buffered registers has been latched, and the blanking period is
 * the time to write the tables in.  Bounded (a pipe that stopped). */
static void color_wait_vblank(struct i915_device *i915, int pipe)
{
	uint32_t f0 = i915_read32(i915, PIPE_FRMCOUNT(pipe));

	for (int t = 0; t < 5000; t++) {
		if (i915_read32(i915, PIPE_FRMCOUNT(pipe)) != f0)
			return;
		lapic_delay_us(20);
	}
}

/* Rewrite the pipe colour bits of register `reg' to `bits' (only those
 * under `mask'); 1 when that changed the register. */
static int color_rmw(struct i915_device *i915, uint32_t reg, uint32_t mask, uint32_t bits)
{
	uint32_t v = i915_read32(i915, reg);
	uint32_t n = (v & ~mask) | (bits & mask);

	if (n == v)
		return 0;
	i915_write32(i915, reg, n);
	return 1;
}

/*
 * Before display 11 the planes carry the pipe's gamma and CSC enables
 * (Gemini Lake in PLANE_COLOR_CTL).  A pipe that stays up but changes
 * them gets the bits rewritten in place, each plane re-armed through its
 * surface register so that they latch with the pipe's own registers at
 * the next vblank.  In a full mode set the plane code writes them itself
 * a moment later; Gemini Lake's PLANE_COLOR_CTL is set here as well,
 * since the plane code writes that register on version 10 only with its
 * plane properties on (I915_FEAT_PLANE_PROPS) -- otherwise PLANE_CTL,
 * whose pipe bits that version ignores.
 */
static void color_update_plane_bits(struct i915_device *i915, struct intel_pipe *p, int modeset)
{
	int ver = i915->info->display_ver, pipe = p->pipe;

	if (ver >= 11)
		return;
	if (ver == 10) {
		uint32_t mask = PLANE_COLOR_PIPE_GAMMA_ENABLE | PLANE_COLOR_PIPE_CSC_ENABLE |
				PLANE_COLOR_PLANE_GAMMA_DISABLE;
		int changed = color_rmw(i915, PLANE_COLOR_CTL(pipe, 0), mask,
					intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_COLOR_CTL));

		if (changed && !modeset &&
		    (i915_read32(i915, PLANE_CTL(pipe, 0)) & PLANE_CTL_ENABLE))
			i915_write32(i915, PLANE_SURF(pipe, 0), i915_read32(i915, PLANE_SURF(pipe, 0)));
	}
	if (modeset)
		return;
	if (ver < 9) {
		uint32_t mask = DISP_PIPE_GAMMA_ENABLE | DISP_PIPE_CSC_ENABLE;

		if ((i915_read32(i915, DSPCNTR(pipe)) & DISP_ENABLE) &&
		    color_rmw(i915, DSPCNTR(pipe), mask,
			      intel_color_plane_bits(i915, p, INTEL_COLOR_REG_DSPCNTR)))
			i915_write32(i915, DSPSURF(pipe), i915_read32(i915, DSPSURF(pipe)));
	} else if (ver == 9) {
		uint32_t mask = PLANE_CTL_PIPE_GAMMA_ENABLE | PLANE_CTL_PIPE_CSC_ENABLE |
				PLANE_CTL_PLANE_GAMMA_DISABLE;

		if ((i915_read32(i915, PLANE_CTL(pipe, 0)) & PLANE_CTL_ENABLE) &&
		    color_rmw(i915, PLANE_CTL(pipe, 0), mask,
			      intel_color_plane_bits(i915, p, INTEL_COLOR_REG_PLANE_CTL)))
			i915_write32(i915, PLANE_SURF(pipe, 0), i915_read32(i915, PLANE_SURF(pipe, 0)));
	}
	{
		uint32_t mask = CUR_GAMMA_ENABLE | MCURSOR_PIPE_CSC_ENABLE;

		if ((i915_read32(i915, CUR_CTL(pipe)) & CUR_MODE_MASK) &&
		    color_rmw(i915, CUR_CTL(pipe), mask,
			      intel_color_plane_bits(i915, p, INTEL_COLOR_REG_CUR_CTL)))
			i915_write32(i915, CUR_BASE(pipe), i915_read32(i915, CUR_BASE(pipe)));
	}
}

/* What p->color records for configuration `c'. */
static struct intel_color_pipe_state color_pipe_state(const struct color_cfg *c)
{
	struct intel_color_pipe_state s;

	mm_memset(&s, 0, sizeof(s));
	if (!c->managed)
		return s;
	s.gamma_mode = c->gamma_mode;
	s.csc_mode = c->csc_mode;
	s.csc_enabled = c->csc_enable || (c->csc_mode & ICL_CSC_ENABLE) != 0;
	s.degamma_enabled = c->pre.present;
	s.managed = 1;
	s.gamma_enabled = c->gamma_enable;
	s.lut_active = c->pre.present || c->post.present;
	return s;
}

/*
 * Program configuration `c' into pipe `p' where a client's configuration
 * is or was in place.  A full mode set (the pipe not showing anything
 * yet): tables, CSC, modes, in that order.  A pipe that stays up: the
 * tables first when the hardware reads none in the old configuration;
 * then the CSC, the modes and the plane bits, all latching together at
 * the next vblank; then -- unless done first -- the tables, once that
 * vblank has started (display 30 on: right away, their tables are
 * double-buffered too).
 */
static void color_program(struct i915_device *i915, struct intel_pipe *p,
			  const struct color_cfg *c, const struct drm_crtc_state *cs, int modeset)
{
	const struct intel_color_pipe_state old = p->color;
	int ver = i915->info->display_ver, pipe = p->pipe;
	int old_reads_table = !old.managed || old.lut_active;
	int loaded = 0, waited = 0;

	if (!modeset && ver == 11 && old.managed && (old.csc_mode & ICL_CSC_ENABLE))
		icl_color_csc_disarm(i915, pipe);

	if (modeset || (c->managed && !old_reads_table && ver < 30)) {
		color_load_luts(i915, pipe, c, cs);
		loaded = 1;
	}

	if (c->managed) {
		color_commit_noarm(i915, pipe, c);
		color_commit_arm(i915, pipe, c);
	} else {
		color_commit_arm_palette(i915, pipe);
	}
	p->color = color_pipe_state(c);
	color_update_plane_bits(i915, p, modeset);

	if (!loaded) {
		if (ver < 30) {
			color_wait_vblank(i915, pipe);
			waited = 1;
		}
		color_load_luts(i915, pipe, c, cs);
	}
	if (waited && ver == 11 && (c->csc_mode & ICL_CSC_ENABLE))
		icl_color_csc_disarm(i915, pipe);
}

/* ---- the entry points intel_display.c calls --------------------------------- */

void intel_color_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	/* Off (or the older displays): the table sizes stay the base
	 * table's (gamma_size 256, no degamma) and no colour feature bit is
	 * set. */
	if (!color_mgmt_on(i915))
		return;
	drv->features |= DRM_FEATURE_COLOR_MGMT;
	/* every pipe handled here has a CSC (display version 5 on) */
	drv->color_has_ctm = i915->info->display_ver >= 5;
	drv->degamma_size = color_degamma_lut_size(i915);
	/* The default GAMMA_LUT_SIZE; the legacy table stays 256 entries
	 * (the crtcs' own size).  Where the core has one size for both, it
	 * stays the legacy one. */
	if (DRM_COLOR_LEGACY_GAMMA_SPLIT)
		drv->gamma_size = color_gamma_lut_size(i915);
}

int intel_color_crtc_init(struct i915_device *i915, int crtc)
{
	struct drm_device *dev = &i915->drm;
	struct drm_crtc *c;
	int rc;

	if (!color_mgmt_on(i915))
		return 0;
	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return -EINVAL;
	c = &dev->crtc[crtc];
	/* SETGAMMA / GETGAMMA keep the 256-entry table on every pipe */
	rc = drm_mode_crtc_set_gamma_size(c, LEGACY_LUT_LENGTH);
	if (rc)
		return rc;
	return drm_crtc_enable_color_mgmt(dev, c, color_degamma_lut_size(i915),
					  i915->info->display_ver >= 5,
					  color_gamma_lut_size(i915));
}

int intel_color_check(struct i915_device *i915, struct intel_output *o,
		      struct drm_crtc_state *cs)
{
	struct color_cfg c;
	const char *why = "a colour table does not fit the pipe";

	(void)o;
	if (!color_mgmt_on(i915))
		return 0;
	/* an unchanged configuration was checked when it was set */
	if (!cs->color_mgmt_changed && !cs->gamma_changed && !drm_crtc_state_needs_modeset(cs))
		return 0;
	if (color_compute(i915, cs, &c, &why))
		return intel_display_check_reject(i915, why);
	if (!c.managed)
		return 0;
	if (drm_color_lut_check(cs->degamma_lut, color_degamma_lut_tests(i915)))
		return intel_display_check_reject(i915, "the degamma table falls or its channels differ");
	if (drm_color_lut_check(cs->gamma_lut, color_gamma_lut_tests(i915, cs->gamma_lut)))
		return intel_display_check_reject(i915, "the gamma table falls");
	return 0;
}

void intel_color_commit(struct i915_device *i915, struct intel_pipe *p,
			const struct drm_crtc_state *cs, int modeset)
{
	struct color_cfg c;
	const char *why = NULL;

	if (!color_mgmt_on(i915)) {
		/* A pipe that stays up gets its table again only when the
		 * client changed it. */
		if (!modeset && !cs->gamma_changed)
			return;
		pipe_gamma_program(i915, p, cs->gamma);
		return;
	}
	if (!modeset && !cs->gamma_changed && !cs->color_mgmt_changed)
		return;
	if (color_compute(i915, cs, &c, &why)) {
		/* the check refuses such a state; shown as the palette */
		WARN_RATELIMIT(1, "i915 pipe %c: colour state not loadable: %s",
			       'A' + p->pipe, why ? why : "?");
		mm_memset(&c, 0, sizeof(c));
	}
	if (!c.managed && !p->color.managed) {
		/* the palette, as it has always been loaded */
		if (!modeset && !cs->gamma_changed)
			return;
		pipe_gamma_program(i915, p, cs->gamma);
		return;
	}
	color_program(i915, p, &c, cs, modeset);
}

uint32_t intel_color_plane_bits(struct i915_device *i915, const struct intel_pipe *p,
				enum intel_color_plane_reg reg)
{
	int ver = i915->info->display_ver;
	uint32_t bits = 0;

	if (!p->color.managed) {
		/* The plane's own gamma stays off and the PIPE's table
		 * applies: that table is the identity until a client sets
		 * one. */
		switch (reg) {
		case INTEL_COLOR_REG_PLANE_CTL:
			return PLANE_CTL_PLANE_GAMMA_DISABLE | PLANE_CTL_PIPE_GAMMA_ENABLE;
		case INTEL_COLOR_REG_PLANE_COLOR_CTL:
			return PLANE_COLOR_PIPE_GAMMA_ENABLE | PLANE_COLOR_PLANE_GAMMA_DISABLE;
		case INTEL_COLOR_REG_DSPCNTR:
			return DISP_PIPE_GAMMA_ENABLE;
		case INTEL_COLOR_REG_CUR_CTL:
			return CUR_GAMMA_ENABLE;
		}
		return 0;
	}

	/* A client's configuration: the pipe's enables as computed for
	 * it.  From display 11 GAMMA_MODE / CSC_MODE enable the stages and
	 * the planes' bits are unused; Gemini Lake keeps them in
	 * PLANE_COLOR_CTL, not PLANE_CTL. */
	switch (reg) {
	case INTEL_COLOR_REG_PLANE_CTL:
		if (ver >= 10)
			return 0;
		bits = PLANE_CTL_PLANE_GAMMA_DISABLE;
		if (p->color.gamma_enabled)
			bits |= PLANE_CTL_PIPE_GAMMA_ENABLE;
		if (p->color.csc_enabled)
			bits |= PLANE_CTL_PIPE_CSC_ENABLE;
		return bits;
	case INTEL_COLOR_REG_PLANE_COLOR_CTL:
		bits = PLANE_COLOR_PLANE_GAMMA_DISABLE;
		if (ver >= 11)
			return bits;
		if (p->color.gamma_enabled)
			bits |= PLANE_COLOR_PIPE_GAMMA_ENABLE;
		if (p->color.csc_enabled)
			bits |= PLANE_COLOR_PIPE_CSC_ENABLE;
		return bits;
	case INTEL_COLOR_REG_DSPCNTR:
		if (p->color.gamma_enabled)
			bits |= DISP_PIPE_GAMMA_ENABLE;
		if (p->color.csc_enabled)
			bits |= DISP_PIPE_CSC_ENABLE;
		return bits;
	case INTEL_COLOR_REG_CUR_CTL:
		if (ver >= 11)
			return 0;
		if (p->color.gamma_enabled)
			bits |= CUR_GAMMA_ENABLE;
		if (p->color.csc_enabled)
			bits |= MCURSOR_PIPE_CSC_ENABLE;
		return bits;
	}
	return 0;
}
