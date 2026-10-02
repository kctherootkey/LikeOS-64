// LikeOS -- display FIFO watermarks and memory self refresh of the parts before DDI.
//
// Every display plane scans out of a small FIFO that the memory arbiter
// refills; the watermark tells the arbiter how empty the FIFO may get
// before it must fetch again, and it has to cover the memory latency at
// the plane's drain rate or the screen tears with underruns.  Each family
// has its own scheme: gen2 and gen3 split one FIFO between the planes
// (DSPARB, fixed by the firmware) and take a single watermark per plane
// in FW_BLC; Pineview and the gen4 parts add self-refresh watermarks in
// DSPFW1-3; G4X adds an HPLL-off level; Valleyview and Cherryview split
// a 511-cacheline FIFO per pipe themselves, take "inverted" watermarks
// and add PM5 and DDR DVFS levels negotiated with the Punit; Ironlake to
// Ivy Bridge program a level-0 watermark per pipe and three low-power
// levels (LP1-LP3) that only work while a single pipe runs.  Memory self
// refresh (CxSR, or the LP levels on the PCH parts) lets the memory sleep
// while one plane runs from a large FIFO, and must be off while planes or
// pipes change.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2022-2026 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

/* ---- registers ------------------------------------------------------------------ */

/* The display FIFO split.  gen2/3: plane A's end (and on i830/i85x plane
 * B's end) in cachelines (i830: half-cachelines); Valleyview/Cherryview:
 * where each pipe's sprites start in its 511-entry FIFO, the ninth bit in
 * DSPARB2. */
#define DSPARB 0x70030u
#define DSPARB_CSTART_SHIFT 7
#define DSPARB_BEND_SHIFT 9 /* i830/i85x */
#define DSPARB_SPRITEA_SHIFT_VLV 0
#define DSPARB_SPRITEA_MASK_VLV (0xffu << 0)
#define DSPARB_SPRITEB_SHIFT_VLV 8
#define DSPARB_SPRITEB_MASK_VLV (0xffu << 8)
#define DSPARB_SPRITEC_SHIFT_VLV 16
#define DSPARB_SPRITEC_MASK_VLV (0xffu << 16)
#define DSPARB_SPRITED_SHIFT_VLV 24
#define DSPARB_SPRITED_MASK_VLV (0xffu << 24)
#define DSPARB2 0x70060u /* VLV/CHV */
#define DSPARB_SPRITEA_HI_SHIFT_VLV 0
#define DSPARB_SPRITEA_HI_MASK_VLV (0x1u << 0)
#define DSPARB_SPRITEB_HI_SHIFT_VLV 4
#define DSPARB_SPRITEB_HI_MASK_VLV (0x1u << 4)
#define DSPARB_SPRITEC_HI_SHIFT_VLV 8
#define DSPARB_SPRITEC_HI_MASK_VLV (0x1u << 8)
#define DSPARB_SPRITED_HI_SHIFT_VLV 12
#define DSPARB_SPRITED_HI_MASK_VLV (0x1u << 12)
#define DSPARB_SPRITEE_HI_SHIFT_VLV 16
#define DSPARB_SPRITEE_HI_MASK_VLV (0x1u << 16)
#define DSPARB_SPRITEF_HI_SHIFT_VLV 20
#define DSPARB_SPRITEF_HI_MASK_VLV (0x1u << 20)
#define DSPARB3 0x7006cu /* CHV pipe C */
#define DSPARB_SPRITEE_SHIFT_VLV 0
#define DSPARB_SPRITEE_MASK_VLV (0xffu << 0)
#define DSPARB_SPRITEF_SHIFT_VLV 8
#define DSPARB_SPRITEF_MASK_VLV (0xffu << 8)

/* Pineview, gen4, G4X, Valleyview, Cherryview watermarks */
#define DSPFW1 0x70034u
#define DSPFW_SR_SHIFT 23
#define DSPFW_SR_MASK (0x1ffu << 23)
#define DSPFW_CURSORB_SHIFT 16
#define DSPFW_CURSORB_MASK (0x3fu << 16)
#define DSPFW_PLANEB_SHIFT 8
#define DSPFW_PLANEB_MASK (0x7fu << 8)
#define DSPFW_PLANEB_MASK_VLV (0xffu << 8)
#define DSPFW_PLANEA_SHIFT 0
#define DSPFW_PLANEA_MASK (0x7fu << 0)
#define DSPFW_PLANEA_MASK_VLV (0xffu << 0)
#define DSPFW2 0x70038u
#define DSPFW_FBC_SR_EN (1u << 31) /* G4X */
#define DSPFW_FBC_SR_SHIFT 28
#define DSPFW_FBC_SR_MASK (0x7u << 28)
#define DSPFW_FBC_HPLL_SR_SHIFT 24
#define DSPFW_FBC_HPLL_SR_MASK (0xfu << 24)
#define DSPFW_SPRITEB_SHIFT 16
#define DSPFW_SPRITEB_MASK (0x7fu << 16)
#define DSPFW_SPRITEB_MASK_VLV (0xffu << 16)
#define DSPFW_CURSORA_SHIFT 8
#define DSPFW_CURSORA_MASK (0x3fu << 8)
#define DSPFW_PLANEC_OLD_SHIFT 0
#define DSPFW_PLANEC_OLD_MASK (0x7fu << 0)
#define DSPFW_SPRITEA_SHIFT 0
#define DSPFW_SPRITEA_MASK (0x7fu << 0)
#define DSPFW_SPRITEA_MASK_VLV (0xffu << 0)
#define DSPFW3 0x7003cu
#define DSPFW_HPLL_SR_EN (1u << 31)
#define PINEVIEW_SELF_REFRESH_EN (1u << 30)
#define DSPFW_CURSOR_SR_SHIFT 24
#define DSPFW_CURSOR_SR_MASK (0x3fu << 24)
#define DSPFW_HPLL_CURSOR_SHIFT 16
#define DSPFW_HPLL_CURSOR_MASK (0x3fu << 16)
#define DSPFW_HPLL_SR_SHIFT 0
#define DSPFW_HPLL_SR_MASK (0x1ffu << 0)

/* Valleyview/Cherryview only */
#define DSPFW4 0x70070u
#define DSPFW5 0x70074u
#define DSPFW6 0x70078u
#define DSPFW7 0x7007cu
#define DSPFW7_CHV 0x700b4u
#define DSPFW8_CHV 0x700b8u
#define DSPFW9_CHV 0x7007cu
#define DSPFW_SPRITED_SHIFT 16
#define DSPFW_SPRITED_MASK_VLV (0xffu << 16)
#define DSPFW_SPRITEC_SHIFT 0
#define DSPFW_SPRITEC_MASK_VLV (0xffu << 0)
#define DSPFW_SPRITEF_SHIFT 16
#define DSPFW_SPRITEF_MASK_VLV (0xffu << 16)
#define DSPFW_SPRITEE_SHIFT 0
#define DSPFW_SPRITEE_MASK_VLV (0xffu << 0)
#define DSPFW_PLANEC_SHIFT 16
#define DSPFW_PLANEC_MASK_VLV (0xffu << 16)
#define DSPFW_CURSORC_SHIFT 0
#define DSPFW_CURSORC_MASK (0x3fu << 0)
/* the high bits of the watermarks above */
#define DSPHOWM 0x70064u
#define DSPHOWM1 0x70068u
#define DSPFW_SR_HI_SHIFT 24
#define DSPFW_SR_HI_MASK (3u << 24) /* two bits on CHV, one on VLV */
#define DSPFW_SPRITEF_HI_SHIFT 23
#define DSPFW_SPRITEF_HI_MASK (1u << 23)
#define DSPFW_SPRITEE_HI_SHIFT 22
#define DSPFW_SPRITEE_HI_MASK (1u << 22)
#define DSPFW_PLANEC_HI_SHIFT 21
#define DSPFW_PLANEC_HI_MASK (1u << 21)
#define DSPFW_SPRITED_HI_SHIFT 20
#define DSPFW_SPRITED_HI_MASK (1u << 20)
#define DSPFW_SPRITEC_HI_SHIFT 16
#define DSPFW_SPRITEC_HI_MASK (1u << 16)
#define DSPFW_PLANEB_HI_SHIFT 12
#define DSPFW_PLANEB_HI_MASK (1u << 12)
#define DSPFW_SPRITEB_HI_SHIFT 8
#define DSPFW_SPRITEB_HI_MASK (1u << 8)
#define DSPFW_SPRITEA_HI_SHIFT 4
#define DSPFW_SPRITEA_HI_MASK (1u << 4)
#define DSPFW_PLANEA_HI_SHIFT 0
#define DSPFW_PLANEA_HI_MASK (1u << 0)
/* drain latency */
#define VLV_DDL(p) (0x70050u + (uint32_t)(p) * 4u)
#define DDL_CURSOR_SHIFT 24
#define DDL_SPRITE_SHIFT(s) (8 + 8 * (s))
#define DDL_PLANE_SHIFT 0
#define DDL_PRECISION_HIGH (1u << 7)
#define DRAIN_LATENCY_MASK 0x7fu

/* gen2/3 watermarks and the self-refresh controls of gen3/4 */
#define FW_BLC 0x20d8u
#define FW_BLC2 0x20dcu
#define FW_BLC_SELF 0x20e0u /* 915 and later */
#define FW_BLC_SELF_FIFO_MASK (1u << 16) /* 945 */
#define FW_BLC_SELF_EN (1u << 15)
#define INSTPM 0x20c0u /* masked */
#define INSTPM_SELF_EN (1u << 12) /* 915GM */

/* Ironlake to Ivy Bridge */
#define WM0_PIPE_ILK(p) ((p) == 2 ? 0x45200u : 0x45100u + (uint32_t)(p) * 4u)
#define WM0_PIPE_PRIMARY(x) (((uint32_t)(x) << 16) & 0xffff0000u)
#define WM0_PIPE_SPRITE(x) (((uint32_t)(x) << 8) & 0x0000ff00u)
#define WM0_PIPE_CURSOR(x) ((uint32_t)(x) & 0x000000ffu)
#define WM1_LP_ILK 0x45108u
#define WM2_LP_ILK 0x4510cu
#define WM3_LP_ILK 0x45110u
#define WM_LP_ENABLE (1u << 31)
#define WM_LP_LATENCY(x) (((uint32_t)(x) << 24) & 0x7f000000u)
#define WM_LP_FBC_ILK(x) (((uint32_t)(x) << 20) & 0x00f00000u)
#define WM_LP_PRIMARY(x) (((uint32_t)(x) << 8) & 0x0007ff00u)
#define WM_LP_CURSOR(x) ((uint32_t)(x) & 0x000000ffu)
#define WM1S_LP_ILK 0x45120u
#define WM2S_LP_IVB 0x45124u
#define WM3S_LP_IVB 0x45128u
#define WM_LP_SPRITE_ENABLE (1u << 31) /* ILK/SNB WM1S only */
#define WM_LP_SPRITE(x) ((uint32_t)(x) & 0x000007ffu)
#define DISP_ARB_CTL2 0x45004u
#define DISP_DATA_PARTITION_5_6 (1u << 6)

/* the memory controller's latencies, in its MCHBAR mirror */
#define MLTR_ILK 0x11222u /* 0.5 us units */
#define MLTR_WM1(v) ((v) & 0x3fu)
#define MLTR_WM2(v) (((v) >> 8) & 0x3fu)
#define MCH_SSKPD_SNB 0x145d10u
#define SSKPD_WM0_SNB(v) ((v) & 0x3fu)
#define SSKPD_WM1_SNB(v) (((v) >> 8) & 0x3fu)
#define SSKPD_WM2_SNB(v) (((v) >> 16) & 0x3fu)
#define SSKPD_WM3_SNB(v) (((v) >> 24) & 0x3fu)

/* Cherryview Punit */
#define PUNIT_REG_DSPSSPM 0x36u
#define DSP_MAXFIFO_PM5_ENABLE (1u << 6)
#define PUNIT_REG_DDR_SETUP2 0x139u
#define FORCE_DDR_FREQ_REQ_ACK (1u << 8)
#define FORCE_DDR_LOW_FREQ (1u << 1)
#define FORCE_DDR_HIGH_FREQ (1u << 0)

/* FIFO geometry */
#define I915_FIFO_LINE_SIZE 64
#define I830_FIFO_LINE_SIZE 32
#define I965_FIFO_SIZE 512
#define I945_FIFO_SIZE 127
#define I915_FIFO_SIZE 95
#define I855GM_FIFO_SIZE 127 /* cachelines */
#define I830_FIFO_SIZE 95
#define I915_MAX_WM 0x3f
#define PINEVIEW_DISPLAY_FIFO 512 /* 64-byte units */
#define PINEVIEW_FIFO_LINE_SIZE 64
#define PINEVIEW_MAX_WM 0x1ff
#define PINEVIEW_DFT_WM 0x3f
#define PINEVIEW_DFT_HPLLOFF_WM 0
#define PINEVIEW_GUARD_WM 10
#define PINEVIEW_CURSOR_FIFO 64
#define PINEVIEW_CURSOR_MAX_WM 0x3f
#define PINEVIEW_CURSOR_DFT_WM 0
#define PINEVIEW_CURSOR_GUARD_WM 5
#define I965_CURSOR_FIFO 64
#define I965_CURSOR_MAX_WM 32
#define I965_CURSOR_DFT_WM 8

#define FW_WM(value, plane)                                                    \
	((((uint32_t)(value)) << DSPFW_##plane##_SHIFT) & DSPFW_##plane##_MASK)
#define FW_WM_VLV(value, plane)                                                \
	((((uint32_t)(value)) << DSPFW_##plane##_SHIFT) & DSPFW_##plane##_MASK_VLV)
#define FW_WM_GET(value, plane)                                                \
	(((value) & DSPFW_##plane##_MASK) >> DSPFW_##plane##_SHIFT)
#define FW_WM_GET_VLV(value, plane)                                            \
	(((value) & DSPFW_##plane##_MASK_VLV) >> DSPFW_##plane##_SHIFT)
#define VLV_FIFO(plane, value)                                                 \
	((((uint32_t)(value)) << DSPARB_##plane##_SHIFT_VLV) & DSPARB_##plane##_MASK_VLV)

/* ---- state ----------------------------------------------------------------------- */

/* Planes of a pipe, in the order the hardware's FIFO split lists them. */
#define WM_PRIMARY 0
#define WM_SPRITE0 1
#define WM_SPRITE1 2
#define WM_CURSOR 3
#define WM_NUM_PLANES 4

/* A level the plane cannot use. */
#define WM_INVALID 0xffffu

enum wm_family {
	WM_FAM_NONE = 0,
	WM_FAM_I845, /* i845G, i865G: one pipe */
	WM_FAM_I9XX, /* i830, i85x, gen3 */
	WM_FAM_PNV,
	WM_FAM_I965,
	WM_FAM_G4X,
	WM_FAM_VLV, /* and Cherryview */
	WM_FAM_ILK, /* Ironlake, Sandy Bridge, Ivy Bridge */
};

struct wm_params {
	uint16_t fifo_size;
	uint16_t max_wm;
	uint8_t default_wm;
	uint8_t guard_size;
	uint8_t cacheline_size;
};

/* What a pipe shows, as the watermarks see it. */
struct wm_in {
	int active; /* the pipe runs */
	int pri_visible; /* its primary plane scans out */
	int cur_visible; /* its cursor does */
	uint32_t pixel_rate; /* kHz */
	uint32_t htotal;
	uint32_t width; /* the primary's width in pixels */
	uint32_t cpp;
	uint32_t cur_w;
	int linear;
};

/* G4X */
#define G4X_LEVEL_NORMAL 0
#define G4X_LEVEL_SR 1
#define G4X_LEVEL_HPLL 2
#define G4X_NUM_LEVELS 3

struct wm_g4x_raw {
	uint16_t plane[WM_NUM_PLANES];
	uint16_t fbc;
};

struct wm_g4x_sr {
	uint16_t plane, cursor, fbc;
};

struct wm_g4x_state {
	uint16_t wm[WM_NUM_PLANES];
	struct wm_g4x_sr sr, hpll;
	int cxsr, hpll_en, fbc_en;
};

/* What is in the registers.  All 16-bit fields: no padding, so two sets
 * compare bytewise. */
struct wm_g4x_values {
	uint16_t pipe[LG_MAX_PIPES][WM_NUM_PLANES];
	struct wm_g4x_sr sr, hpll;
	uint16_t cxsr, hpll_en, fbc_en;
};

/* Valleyview / Cherryview */
#define VLV_LEVEL_PM2 0
#define VLV_LEVEL_PM5 1
#define VLV_LEVEL_DDR_DVFS 2
#define VLV_NUM_LEVELS 3

struct wm_vlv_sr {
	uint16_t plane, cursor;
};

struct wm_vlv_state {
	uint16_t wm[VLV_NUM_LEVELS][WM_NUM_PLANES];
	struct wm_vlv_sr sr[VLV_NUM_LEVELS];
	int num_levels;
	int cxsr;
};

struct wm_vlv_fifo {
	uint16_t plane[WM_NUM_PLANES];
};

struct wm_vlv_values {
	uint16_t pipe[LG_MAX_PIPES][WM_NUM_PLANES];
	struct wm_vlv_sr sr;
	uint8_t ddl[LG_MAX_PIPES][WM_NUM_PLANES];
	uint16_t level;
	uint16_t cxsr;
};

/* Ironlake to Ivy Bridge */
#define ILK_MAX_LEVELS 5
#define ILK_DDB_PART_1_2 0
#define ILK_DDB_PART_5_6 1

struct wm_ilk_level {
	int enable;
	uint32_t pri_val, spr_val, cur_val, fbc_val;
};

struct wm_ilk_pipe {
	int pipe_enabled;
	int sprites_enabled, sprites_scaled;
	int fbc_wm_enabled; /* merged only */
	struct wm_ilk_level wm[ILK_MAX_LEVELS];
};

struct wm_ilk_values {
	uint32_t wm_pipe[LG_MAX_PIPES];
	uint32_t wm_lp[3];
	uint32_t wm_lp_spr[3];
	int enable_fbc_wm;
	int partitioning;
};

struct wm_ilk_max {
	uint32_t pri, spr, cur, fbc;
};

struct wm_ilk_config {
	int num_pipes_active;
	int sprites_enabled;
	int sprites_scaled;
};

/* Pineview self-refresh latencies (ns) by memory configuration. */
struct wm_cxsr_latency {
	uint8_t is_desktop;
	uint8_t is_ddr3;
	uint16_t fsb_freq; /* MHz */
	uint16_t mem_freq; /* MHz */
	uint16_t display_sr;
	uint16_t display_hpll_disable;
	uint16_t cursor_sr;
	uint16_t cursor_hpll_disable;
};

static struct {
	enum wm_family family;
	/* latencies: G4X/VLV in us, ILK-IVB level 0 in 0.1 us and LP1+
	 * in 0.5 us */
	uint16_t pri_latency[ILK_MAX_LEVELS];
	uint16_t spr_latency[ILK_MAX_LEVELS];
	uint16_t cur_latency[ILK_MAX_LEVELS];
	int num_levels;
	const struct wm_cxsr_latency *pnv_latency;
	struct wm_g4x_values g4x;
	struct wm_vlv_values vlv;
	struct wm_vlv_fifo vlv_fifo[LG_MAX_PIPES];
	int vlv_fifo_valid;
	int vlv_valid;
	int g4x_valid;
	struct wm_ilk_values ilk_hw;
} g_wm;

static const struct wm_cxsr_latency cxsr_latency_table[] = {
	{ 1, 0, 800, 400, 3382, 33382, 3983, 33983 }, /* DDR2-400 SC */
	{ 1, 0, 800, 667, 3354, 33354, 3807, 33807 }, /* DDR2-667 SC */
	{ 1, 0, 800, 800, 3347, 33347, 3763, 33763 }, /* DDR2-800 SC */
	{ 1, 1, 800, 667, 6420, 36420, 6873, 36873 }, /* DDR3-667 SC */
	{ 1, 1, 800, 800, 5902, 35902, 6318, 36318 }, /* DDR3-800 SC */

	{ 1, 0, 667, 400, 3400, 33400, 4021, 34021 }, /* DDR2-400 SC */
	{ 1, 0, 667, 667, 3372, 33372, 3845, 33845 }, /* DDR2-667 SC */
	{ 1, 0, 667, 800, 3386, 33386, 3822, 33822 }, /* DDR2-800 SC */
	{ 1, 1, 667, 667, 6438, 36438, 6911, 36911 }, /* DDR3-667 SC */
	{ 1, 1, 667, 800, 5941, 35941, 6377, 36377 }, /* DDR3-800 SC */

	{ 1, 0, 400, 400, 3472, 33472, 4173, 34173 }, /* DDR2-400 SC */
	{ 1, 0, 400, 667, 3443, 33443, 3996, 33996 }, /* DDR2-667 SC */
	{ 1, 0, 400, 800, 3430, 33430, 3946, 33946 }, /* DDR2-800 SC */
	{ 1, 1, 400, 667, 6509, 36509, 7062, 37062 }, /* DDR3-667 SC */
	{ 1, 1, 400, 800, 5985, 35985, 6501, 36501 }, /* DDR3-800 SC */

	{ 0, 0, 800, 400, 3438, 33438, 4065, 34065 }, /* DDR2-400 SC */
	{ 0, 0, 800, 667, 3410, 33410, 3889, 33889 }, /* DDR2-667 SC */
	{ 0, 0, 800, 800, 3403, 33403, 3845, 33845 }, /* DDR2-800 SC */
	{ 0, 1, 800, 667, 6476, 36476, 6955, 36955 }, /* DDR3-667 SC */
	{ 0, 1, 800, 800, 5958, 35958, 6400, 36400 }, /* DDR3-800 SC */

	{ 0, 0, 667, 400, 3456, 33456, 4103, 34106 }, /* DDR2-400 SC */
	{ 0, 0, 667, 667, 3428, 33428, 3927, 33927 }, /* DDR2-667 SC */
	{ 0, 0, 667, 800, 3443, 33443, 3905, 33905 }, /* DDR2-800 SC */
	{ 0, 1, 667, 667, 6494, 36494, 6993, 36993 }, /* DDR3-667 SC */
	{ 0, 1, 667, 800, 5998, 35998, 6460, 36460 }, /* DDR3-800 SC */

	{ 0, 0, 400, 400, 3528, 33528, 4255, 34255 }, /* DDR2-400 SC */
	{ 0, 0, 400, 667, 3500, 33500, 4079, 34079 }, /* DDR2-667 SC */
	{ 0, 0, 400, 800, 3487, 33487, 4029, 34029 }, /* DDR2-800 SC */
	{ 0, 1, 400, 667, 6566, 36566, 7145, 37145 }, /* DDR3-667 SC */
	{ 0, 1, 400, 800, 6042, 36042, 6584, 36584 }, /* DDR3-800 SC */
};

/* Pineview has different values for various configs */
static const struct wm_params pnv_display_wm = {
	.fifo_size = PINEVIEW_DISPLAY_FIFO,
	.max_wm = PINEVIEW_MAX_WM,
	.default_wm = PINEVIEW_DFT_WM,
	.guard_size = PINEVIEW_GUARD_WM,
	.cacheline_size = PINEVIEW_FIFO_LINE_SIZE,
};

static const struct wm_params pnv_display_hplloff_wm = {
	.fifo_size = PINEVIEW_DISPLAY_FIFO,
	.max_wm = PINEVIEW_MAX_WM,
	.default_wm = PINEVIEW_DFT_HPLLOFF_WM,
	.guard_size = PINEVIEW_GUARD_WM,
	.cacheline_size = PINEVIEW_FIFO_LINE_SIZE,
};

static const struct wm_params pnv_cursor_wm = {
	.fifo_size = PINEVIEW_CURSOR_FIFO,
	.max_wm = PINEVIEW_CURSOR_MAX_WM,
	.default_wm = PINEVIEW_CURSOR_DFT_WM,
	.guard_size = PINEVIEW_CURSOR_GUARD_WM,
	.cacheline_size = PINEVIEW_FIFO_LINE_SIZE,
};

static const struct wm_params pnv_cursor_hplloff_wm = {
	.fifo_size = PINEVIEW_CURSOR_FIFO,
	.max_wm = PINEVIEW_CURSOR_MAX_WM,
	.default_wm = PINEVIEW_CURSOR_DFT_WM,
	.guard_size = PINEVIEW_CURSOR_GUARD_WM,
	.cacheline_size = PINEVIEW_FIFO_LINE_SIZE,
};

static const struct wm_params i965_cursor_wm_info = {
	.fifo_size = I965_CURSOR_FIFO,
	.max_wm = I965_CURSOR_MAX_WM,
	.default_wm = I965_CURSOR_DFT_WM,
	.guard_size = 2,
	.cacheline_size = I915_FIFO_LINE_SIZE,
};

static const struct wm_params i945_wm_info = {
	.fifo_size = I945_FIFO_SIZE,
	.max_wm = I915_MAX_WM,
	.default_wm = 1,
	.guard_size = 2,
	.cacheline_size = I915_FIFO_LINE_SIZE,
};

static const struct wm_params i915_wm_info = {
	.fifo_size = I915_FIFO_SIZE,
	.max_wm = I915_MAX_WM,
	.default_wm = 1,
	.guard_size = 2,
	.cacheline_size = I915_FIFO_LINE_SIZE,
};

static const struct wm_params i830_a_wm_info = {
	.fifo_size = I855GM_FIFO_SIZE,
	.max_wm = I915_MAX_WM,
	.default_wm = 1,
	.guard_size = 2,
	.cacheline_size = I830_FIFO_LINE_SIZE,
};

static const struct wm_params i830_bc_wm_info = {
	.fifo_size = I855GM_FIFO_SIZE,
	.max_wm = I915_MAX_WM / 2,
	.default_wm = 1,
	.guard_size = 2,
	.cacheline_size = I830_FIFO_LINE_SIZE,
};

static const struct wm_params i845_wm_info = {
	.fifo_size = I830_FIFO_SIZE,
	.max_wm = I915_MAX_WM,
	.default_wm = 1,
	.guard_size = 2,
	.cacheline_size = I830_FIFO_LINE_SIZE,
};

/*
 * The latency of a FIFO fetch depends on the memory configuration, the
 * chipset and what state the memory controller is in.  It can be high,
 * so the gen2/3 watermarks assume a pessimal value: too high and the
 * FIFO refills more often than it needs to, too low and it underruns.
 * 5 us is safe for the slowest parts without being wasteful on the rest.
 */
#define PESSIMAL_LATENCY_NS 5000

/* ---- helpers --------------------------------------------------------------------- */

static char wm_pipe_name(int pipe)
{
	return (char)('A' + pipe);
}

static int wm_same(const void *a, const void *b, uint32_t len)
{
	const uint8_t *x = a, *y = b;
	uint32_t i;

	for (i = 0; i < len; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static uint32_t wm_min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint32_t wm_max_u32(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

static int is_disabling(int old, int new, int threshold)
{
	return old >= threshold && new < threshold;
}

static int is_enabling(int old, int new, int threshold)
{
	return old < threshold && new >= threshold;
}

static void wm_get_in(struct lg_display *d, int pipe, struct wm_in *in)
{
	const struct lg_pipe *p = &d->pipes[pipe];

	mm_memset(in, 0, sizeof(*in));
	if (pipe >= d->num_pipes || !p->active)
		return;

	in->active = 1;
	in->pixel_rate = p->cfg.pixel_rate ? p->cfg.pixel_rate : p->cfg.t.clock;
	in->htotal = p->cfg.t.htotal;
	in->width = p->width;
	in->cpp = p->cpp;
	in->linear = p->modifier == DRM_FORMAT_MOD_LINEAR;
	in->pri_visible = p->plane_enabled && p->width && p->cpp;
	in->cur_visible = p->cursor_enabled && p->cursor_w;
	in->cur_w = p->cursor_w;
}

/* A pipe that scans a plane out at a known clock (gen2-4). */
static int wm_crtc_active(struct lg_display *d, int pipe)
{
	struct wm_in in;

	if (pipe < 0)
		return 0;
	wm_get_in(d, pipe, &in);
	return in.active && in.pri_visible && in.pixel_rate;
}

/* The one active pipe, or -1 when none or several are. */
static int wm_single_enabled_pipe(struct lg_display *d)
{
	int pipe, enabled = -1;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		if (wm_crtc_active(d, pipe)) {
			if (enabled >= 0)
				return -1;
			enabled = pipe;
		}
	}
	return enabled;
}

/* The pipe primary plane `plane' feeds (gen2/3), -1 none. */
static int wm_pipe_for_plane(struct lg_display *d, int plane)
{
	int pipe;

	for (pipe = 0; pipe < d->num_pipes; pipe++)
		if (d->pipes[pipe].plane == plane)
			return pipe;
	return -1;
}

/*
 * Method 1, the "small buffer" formula: the bytes the plane drains during
 * the latency (0.1 us units) at its pixel rate.  It looks at the short
 * term drain rate only, ignoring the blanking that lowers the average.
 */
static uint32_t wm_method1(uint32_t pixel_rate, uint32_t cpp, uint32_t latency)
{
	uint64_t ret;

	ret = (uint64_t)pixel_rate * (uint64_t)(cpp * latency);
	ret = (ret + 10000 - 1) / 10000;
	return (uint32_t)ret;
}

/*
 * Method 2, the "large buffer" formula: whole lines' worth of bytes for
 * the lines that pass during the latency, which accounts for blanking
 * and so for the long term drain rate.
 */
static uint32_t wm_method2(uint32_t pixel_rate, uint32_t htotal, uint32_t width,
			   uint32_t cpp, uint32_t latency)
{
	uint32_t ret;

	if (htotal == 0)
		htotal = 1;

	ret = (latency * pixel_rate) / (htotal * 10000);
	ret = (ret + 1) * width * cpp;
	return ret;
}

/*
 * The FIFO level at which the plane starts fetching again: the FIFO size
 * minus what drains during the latency, in the chip's cachelines, clamped
 * to what the register holds.
 */
static int wm_calculate(uint32_t pixel_rate, const struct wm_params *wm, int fifo_size,
			int cpp, uint32_t latency_ns)
{
	int entries, wm_size;

	entries = (int)wm_method1(pixel_rate, (uint32_t)cpp, latency_ns / 100);
	entries = DIV_ROUND_UP(entries, (int)wm->cacheline_size) + wm->guard_size;
	i915_dbg("[drm] i915: wm: FIFO entries required for mode: %d\n", entries);

	wm_size = fifo_size - entries;
	i915_dbg("[drm] i915: wm: FIFO watermark level: %d\n", wm_size);

	if (wm_size > (int)wm->max_wm)
		wm_size = wm->max_wm;
	if (wm_size <= 0)
		wm_size = wm->default_wm;

	/* The value should not go below the burst size plus one; the i830
	 * is quite unhappy with low values.  8 is the burst size. */
	if (wm_size <= 8)
		wm_size = 8;

	return wm_size;
}

/* ---- self refresh ------------------------------------------------------------------ */

/* Allow or forbid CxSR in the hardware; returns whether it was allowed. */
static int wm_set_cxsr_hw(struct lg_display *d, int enable)
{
	int was_enabled;
	uint32_t val;

	if (d->is_vlv || d->is_chv) {
		was_enabled = !!(lg_rd(d, FW_BLC_SELF_VLV) & FW_CSPWRDWNEN);
		lg_wr(d, FW_BLC_SELF_VLV, enable ? FW_CSPWRDWNEN : 0);
		lg_posting_read(d, FW_BLC_SELF_VLV);
	} else if (d->is_g4x || d->is_gm45 || d->is_i965gm) {
		was_enabled = !!(lg_rd(d, FW_BLC_SELF) & FW_BLC_SELF_EN);
		lg_wr(d, FW_BLC_SELF, enable ? FW_BLC_SELF_EN : 0);
		lg_posting_read(d, FW_BLC_SELF);
	} else if (d->is_pnv) {
		val = lg_rd(d, DSPFW3);
		was_enabled = !!(val & PINEVIEW_SELF_REFRESH_EN);
		if (enable)
			val |= PINEVIEW_SELF_REFRESH_EN;
		else
			val &= ~PINEVIEW_SELF_REFRESH_EN;
		lg_wr(d, DSPFW3, val);
		lg_posting_read(d, DSPFW3);
	} else if (d->is_i945g || d->is_i945gm) {
		was_enabled = !!(lg_rd(d, FW_BLC_SELF) & FW_BLC_SELF_EN);
		val = enable ? LG_MASKED_ENABLE(FW_BLC_SELF_EN) :
			       LG_MASKED_DISABLE(FW_BLC_SELF_EN);
		lg_wr(d, FW_BLC_SELF, val);
		lg_posting_read(d, FW_BLC_SELF);
	} else if (d->is_i915gm) {
		/* The 915GM's self-refresh enable sits in INSTPM; the 915G
		 * has the watermark in FW_BLC_SELF but no such bit. */
		was_enabled = !!(lg_rd(d, INSTPM) & INSTPM_SELF_EN);
		val = enable ? LG_MASKED_ENABLE(INSTPM_SELF_EN) :
			       LG_MASKED_DISABLE(INSTPM_SELF_EN);
		lg_wr(d, INSTPM, val);
		lg_posting_read(d, INSTPM);
	} else {
		return 0;
	}

	i915_dbg("[drm] i915: memory self-refresh is %s (was %s)\n",
		 enable ? "enabled" : "disabled", was_enabled ? "enabled" : "disabled");
	return was_enabled;
}

/*
 * Allow or forbid CxSR, the C-state self refresh in which the display
 * FIFOs may be combined into one large FIFO for a single plane ("max
 * FIFO") so that memory fetches can be deferred and the memory can sleep.
 * While the system is in that mode some plane registers do not latch on
 * vblank, so it must be forbidden before plane registers change -- and
 * since the hardware re-evaluates the condition only at the next frame
 * start, the change must wait a frame after that.  On Valleyview and
 * Cherryview this bit only controls max FIFO; the memory may self refresh
 * regardless.  The deeper HPLL-off mode depends on CxSR, so forbidding
 * CxSR covers it.
 */
static int wm_set_cxsr(struct lg_display *d, int enable)
{
	int ret = wm_set_cxsr_hw(d, enable);

	if (d->is_vlv || d->is_chv)
		g_wm.vlv.cxsr = (uint16_t)!!enable;
	else if (d->is_g4x || d->is_gm45)
		g_wm.g4x.cxsr = (uint16_t)!!enable;
	return ret;
}

/* ---- gen2 / gen3 ----------------------------------------------------------------- */

static int wm_i9xx_get_fifo_size(struct lg_display *d, int plane)
{
	uint32_t dsparb = lg_rd(d, DSPARB);
	int size;

	size = (int)(dsparb & 0x7f);
	if (plane == 1)
		size = (int)((dsparb >> DSPARB_CSTART_SHIFT) & 0x7f) - size;

	i915_dbg("[drm] i915: wm: FIFO size - (0x%08x) %c: %d\n", dsparb,
		 wm_pipe_name(plane), size);
	return size;
}

static int wm_i830_get_fifo_size(struct lg_display *d, int plane)
{
	uint32_t dsparb = lg_rd(d, DSPARB);
	int size;

	size = (int)(dsparb & 0x1ff);
	if (plane == 1)
		size = (int)((dsparb >> DSPARB_BEND_SHIFT) & 0x1ff) - size;
	size >>= 1; /* to cachelines */

	i915_dbg("[drm] i915: wm: FIFO size - (0x%08x) %c: %d\n", dsparb,
		 wm_pipe_name(plane), size);
	return size;
}

static int wm_i845_get_fifo_size(struct lg_display *d, int plane)
{
	uint32_t dsparb = lg_rd(d, DSPARB);
	int size;

	size = (int)(dsparb & 0x7f);
	size >>= 2; /* to cachelines */

	i915_dbg("[drm] i915: wm: FIFO size - (0x%08x) %c: %d\n", dsparb,
		 wm_pipe_name(plane), size);
	return size;
}

/* The watermark of one gen2/3 primary plane. */
static int wm_i9xx_plane_wm(struct lg_display *d, int plane, const struct wm_params *wm_info,
			    int fifo_size)
{
	int pipe = wm_pipe_for_plane(d, plane);
	int plane_wm;

	if (wm_crtc_active(d, pipe)) {
		struct wm_in in;
		int cpp;

		wm_get_in(d, pipe, &in);
		cpp = d->ver == 2 ? 4 : (int)in.cpp;
		plane_wm = wm_calculate(in.pixel_rate, wm_info, fifo_size, cpp,
					PESSIMAL_LATENCY_NS);
	} else {
		plane_wm = fifo_size - wm_info->guard_size;
		if (plane_wm > (int)wm_info->max_wm)
			plane_wm = wm_info->max_wm;
	}
	return plane_wm;
}

static void wm_i9xx_update(struct lg_display *d)
{
	const struct wm_params *wm_info;
	uint32_t fwater_lo, fwater_hi;
	int cwm, srwm = 1;
	int fifo_size;
	int planea_wm, planeb_wm;
	int pipe;

	if (d->is_i945gm)
		wm_info = &i945_wm_info;
	else if (d->ver != 2)
		wm_info = &i915_wm_info;
	else
		wm_info = &i830_a_wm_info;

	if (d->ver == 2)
		fifo_size = wm_i830_get_fifo_size(d, 0);
	else
		fifo_size = wm_i9xx_get_fifo_size(d, 0);
	planea_wm = wm_i9xx_plane_wm(d, 0, wm_info, fifo_size);

	if (d->ver == 2)
		wm_info = &i830_bc_wm_info;

	if (d->ver == 2)
		fifo_size = wm_i830_get_fifo_size(d, 1);
	else
		fifo_size = wm_i9xx_get_fifo_size(d, 1);
	planeb_wm = wm_i9xx_plane_wm(d, 1, wm_info, fifo_size);

	i915_dbg("[drm] i915: wm: FIFO watermarks - A: %d, B: %d\n", planea_wm, planeb_wm);

	pipe = wm_single_enabled_pipe(d);
	if (d->is_i915gm && pipe >= 0) {
		/* self refresh is broken with an untiled plane */
		if (d->pipes[pipe].modifier == DRM_FORMAT_MOD_LINEAR)
			pipe = -1;
	}

	/* The overlay gets an aggressive default: video jitter is bad. */
	cwm = 2;

	/* Self refresh off while the watermarks change. */
	wm_set_cxsr(d, 0);

	/* The self-refresh watermark for a single plane (gen3). */
	if (d->ver >= 3 && pipe >= 0) {
		/* self refresh has a much higher latency */
		const uint32_t sr_latency_ns = 6000;
		struct wm_in in;
		int cpp, entries;

		wm_get_in(d, pipe, &in);
		if (d->is_i915gm || d->is_i945gm)
			cpp = 4;
		else
			cpp = (int)in.cpp;

		entries = (int)wm_method2(in.pixel_rate, in.htotal, in.width, (uint32_t)cpp,
					  sr_latency_ns / 100);
		entries = DIV_ROUND_UP(entries, (int)wm_info->cacheline_size);
		i915_dbg("[drm] i915: wm: self-refresh entries: %d\n", entries);
		srwm = (int)wm_info->fifo_size - entries;
		if (srwm < 0)
			srwm = 1;

		if (d->is_i945g || d->is_i945gm)
			lg_wr(d, FW_BLC_SELF, FW_BLC_SELF_FIFO_MASK | ((uint32_t)srwm & 0xff));
		else
			lg_wr(d, FW_BLC_SELF, (uint32_t)srwm & 0x3f);
	}

	i915_dbg("[drm] i915: wm: setting FIFO watermarks - A: %d, B: %d, C: %d, SR %d\n",
		 planea_wm, planeb_wm, cwm, srwm);

	fwater_lo = (((uint32_t)planeb_wm & 0x3f) << 16) | ((uint32_t)planea_wm & 0x3f);
	fwater_hi = (uint32_t)cwm & 0x1f;

	/* request 8 cachelines per fetch */
	fwater_lo = fwater_lo | (1u << 24) | (1u << 8);
	fwater_hi = fwater_hi | (1u << 8);

	lg_wr(d, FW_BLC, fwater_lo);
	lg_wr(d, FW_BLC2, fwater_hi);

	if (pipe >= 0)
		wm_set_cxsr(d, 1);
}

static void wm_i845_update(struct lg_display *d)
{
	uint32_t fwater_lo;
	int planea_wm;
	int pipe;
	struct wm_in in;

	pipe = wm_single_enabled_pipe(d);
	if (pipe < 0)
		return;

	wm_get_in(d, pipe, &in);
	planea_wm = wm_calculate(in.pixel_rate, &i845_wm_info, wm_i845_get_fifo_size(d, 0),
				 4, PESSIMAL_LATENCY_NS);
	fwater_lo = lg_rd(d, FW_BLC) & ~0xfffu;
	fwater_lo |= (3u << 8) | (uint32_t)planea_wm;

	i915_dbg("[drm] i915: wm: setting FIFO watermarks - A: %d\n", planea_wm);

	lg_wr(d, FW_BLC, fwater_lo);
}

/* ---- Pineview ------------------------------------------------------------------- */

static const struct wm_cxsr_latency *wm_pnv_get_cxsr_latency(struct lg_display *d)
{
	uint32_t fsb = DIV_ROUND_CLOSEST(d->fsb_khz, 1000);
	uint32_t mem = DIV_ROUND_CLOSEST(d->mem_khz, 1000);
	int is_desktop = !d->mobile;
	int is_ddr3 = !!d->is_ddr3;
	uint32_t i;

	for (i = 0; i < sizeof(cxsr_latency_table) / sizeof(cxsr_latency_table[0]); i++) {
		const struct wm_cxsr_latency *latency = &cxsr_latency_table[i];

		if (is_desktop == latency->is_desktop && is_ddr3 == latency->is_ddr3 &&
		    fsb == latency->fsb_freq && mem == latency->mem_freq)
			return latency;
	}

	i915_dbg("[drm] i915: wm: no CxSR latency for %s, FSB %u kHz, MEM %u kHz\n",
		 is_ddr3 ? "DDR3" : "DDR2", d->fsb_khz, d->mem_khz);
	return NULL;
}

static void wm_pnv_update(struct lg_display *d)
{
	const struct wm_cxsr_latency *latency = g_wm.pnv_latency;
	uint32_t reg;
	int pipe, wm;

	if (!latency) {
		i915_dbg("[drm] i915: wm: unknown FSB/MEM, disabling CxSR\n");
		wm_set_cxsr(d, 0);
		return;
	}

	pipe = wm_single_enabled_pipe(d);
	if (pipe >= 0) {
		struct wm_in in;
		uint32_t pixel_rate;
		int cpp;

		wm_get_in(d, pipe, &in);
		pixel_rate = in.pixel_rate;
		cpp = (int)in.cpp;

		/* display SR */
		wm = wm_calculate(pixel_rate, &pnv_display_wm, pnv_display_wm.fifo_size, cpp,
				  latency->display_sr);
		reg = lg_rd(d, DSPFW1);
		reg &= ~DSPFW_SR_MASK;
		reg |= FW_WM(wm, SR);
		lg_wr(d, DSPFW1, reg);
		i915_dbg("[drm] i915: wm: DSPFW1 register is %x\n", reg);

		/* cursor SR */
		wm = wm_calculate(pixel_rate, &pnv_cursor_wm, pnv_display_wm.fifo_size, 4,
				  latency->cursor_sr);
		lg_rmw(d, DSPFW3, DSPFW_CURSOR_SR_MASK, FW_WM(wm, CURSOR_SR));

		/* display HPLL off SR */
		wm = wm_calculate(pixel_rate, &pnv_display_hplloff_wm,
				  pnv_display_hplloff_wm.fifo_size, cpp,
				  latency->display_hpll_disable);
		lg_rmw(d, DSPFW3, DSPFW_HPLL_SR_MASK, FW_WM(wm, HPLL_SR));

		/* cursor HPLL off SR */
		wm = wm_calculate(pixel_rate, &pnv_cursor_hplloff_wm,
				  pnv_display_hplloff_wm.fifo_size, 4,
				  latency->cursor_hpll_disable);
		reg = lg_rd(d, DSPFW3);
		reg &= ~DSPFW_HPLL_CURSOR_MASK;
		reg |= FW_WM(wm, HPLL_CURSOR);
		lg_wr(d, DSPFW3, reg);
		i915_dbg("[drm] i915: wm: DSPFW3 register is %x\n", reg);

		wm_set_cxsr(d, 1);
	} else {
		wm_set_cxsr(d, 0);
	}
}

/* ---- gen4 (i965G, i965GM) ------------------------------------------------------------ */

static void wm_i965_update(struct lg_display *d)
{
	int srwm = 1;
	int cursor_sr = 16;
	int cxsr_enabled;
	int pipe;

	/* the self-refresh watermarks for a single plane */
	pipe = wm_single_enabled_pipe(d);
	if (pipe >= 0) {
		/* self refresh has a much higher latency */
		const uint32_t sr_latency_ns = 12000;
		struct wm_in in;
		int entries;

		wm_get_in(d, pipe, &in);
		entries = (int)wm_method2(in.pixel_rate, in.htotal, in.width, in.cpp,
					  sr_latency_ns / 100);
		entries = DIV_ROUND_UP(entries, I915_FIFO_LINE_SIZE);
		srwm = I965_FIFO_SIZE - entries;
		if (srwm < 0)
			srwm = 1;
		srwm &= 0x1ff;
		i915_dbg("[drm] i915: wm: self-refresh entries: %d, wm: %d\n", entries, srwm);

		entries = (int)wm_method2(in.pixel_rate, in.htotal,
					  in.cur_visible ? in.cur_w : 0, 4,
					  sr_latency_ns / 100);
		entries = DIV_ROUND_UP(entries, (int)i965_cursor_wm_info.cacheline_size) +
			  i965_cursor_wm_info.guard_size;

		cursor_sr = (int)i965_cursor_wm_info.fifo_size - entries;
		if (cursor_sr > (int)i965_cursor_wm_info.max_wm)
			cursor_sr = i965_cursor_wm_info.max_wm;

		i915_dbg("[drm] i915: wm: self-refresh watermark: display plane %d cursor %d\n",
			 srwm, cursor_sr);

		cxsr_enabled = 1;
	} else {
		cxsr_enabled = 0;
		/* self refresh off while more than one pipe runs */
		wm_set_cxsr(d, 0);
	}

	i915_dbg("[drm] i915: wm: setting FIFO watermarks - A: 8, B: 8, C: 8, SR %d\n", srwm);

	/* the 965 takes fixed plane watermarks */
	lg_wr(d, DSPFW1, FW_WM(srwm, SR) | FW_WM(8, CURSORB) | FW_WM(8, PLANEB) |
				 FW_WM(8, PLANEA));
	lg_wr(d, DSPFW2, FW_WM(8, CURSORA) | FW_WM(8, PLANEC_OLD));
	/* the cursor's self-refresh watermark */
	lg_wr(d, DSPFW3, FW_WM(cursor_sr, CURSOR_SR));

	if (cxsr_enabled)
		wm_set_cxsr(d, 1);
}

/* ---- G4X -------------------------------------------------------------------------- */

static void wm_g4x_setup_latency(void)
{
	/* in us */
	g_wm.pri_latency[G4X_LEVEL_NORMAL] = 5;
	g_wm.pri_latency[G4X_LEVEL_SR] = 12;
	g_wm.pri_latency[G4X_LEVEL_HPLL] = 35;
	g_wm.num_levels = G4X_LEVEL_HPLL + 1;
}

/*
 * DSPCNTR bit 13 supposedly lets the primary plane use the sprite's FIFO
 * space, but both the primary and sprite watermarks can be set up to 127
 * cachelines either way, so the repartitioning does not matter: the
 * largest value the registers hold is below the smallest FIFO.
 */
static uint32_t wm_g4x_plane_fifo_size(int plane, int level)
{
	switch (plane) {
	case WM_CURSOR:
		return 63;
	case WM_PRIMARY:
		return level == G4X_LEVEL_NORMAL ? 127 : 511;
	case WM_SPRITE0:
		return level == G4X_LEVEL_NORMAL ? 127 : 0;
	default:
		return 0;
	}
}

static uint32_t wm_g4x_fbc_fifo_size(int level)
{
	switch (level) {
	case G4X_LEVEL_SR:
		return 7;
	case G4X_LEVEL_HPLL:
		return 15;
	default:
		return 0;
	}
}

/*
 * When eight whole lines fit in the FIFO, TLB fetches can get in the way
 * of the data fetches and delay them beyond what the formulas allow; the
 * watermark is then raised by the difference between the FIFO size and
 * eight lines, always at the plane's real pixel depth.
 */
static uint32_t wm_g4x_tlb_miss_wa(uint32_t fifo_size, uint32_t width, uint32_t cpp)
{
	int64_t tlb_miss = (int64_t)fifo_size * 64 - (int64_t)width * cpp * 8;

	return tlb_miss > 0 ? (uint32_t)tlb_miss : 0;
}

static int wm_plane_visible(const struct wm_in *in, int plane)
{
	if (!in->active)
		return 0;
	if (plane == WM_PRIMARY)
		return in->pri_visible;
	if (plane == WM_CURSOR)
		return in->cur_visible;
	return 0; /* no sprites */
}

static uint16_t wm_g4x_compute_wm(const struct wm_in *in, int plane, int level)
{
	uint32_t latency = (uint32_t)g_wm.pri_latency[level] * 10;
	uint32_t pixel_rate, htotal, cpp, width, wm;

	if (latency == 0)
		return WM_INVALID;

	if (!wm_plane_visible(in, plane))
		return 0;

	cpp = plane == WM_CURSOR ? 4 : in->cpp;

	/* The self-refresh watermarks take 32 bpp (WaUse32BppForSRWM);
	 * the HPLL one as well, to be safe. */
	if (plane == WM_PRIMARY && level != G4X_LEVEL_NORMAL)
		cpp = wm_max_u32(cpp, 4);

	pixel_rate = in->pixel_rate;
	htotal = in->htotal;
	width = plane == WM_CURSOR ? in->cur_w : in->width;

	if (plane == WM_CURSOR) {
		wm = wm_method2(pixel_rate, htotal, width, cpp, latency);
	} else if (plane == WM_PRIMARY && level == G4X_LEVEL_NORMAL) {
		wm = wm_method1(pixel_rate, cpp, latency);
	} else {
		uint32_t small, large;

		small = wm_method1(pixel_rate, cpp, latency);
		large = wm_method2(pixel_rate, htotal, width, cpp, latency);
		wm = wm_min_u32(small, large);
	}

	wm += wm_g4x_tlb_miss_wa(wm_g4x_plane_fifo_size(plane, level), width, cpp);
	wm = DIV_ROUND_UP(wm, 64) + 2;

	return (uint16_t)wm_min_u32(wm, WM_INVALID);
}

/* The FBC watermark that goes with a primary watermark (LP levels). */
static uint32_t wm_compute_fbc_wm(const struct wm_in *in, uint32_t pri_val)
{
	uint32_t horiz_pixels = in->width, cpp = in->cpp;

	if (!wm_plane_visible(in, WM_PRIMARY))
		return 0;
	if (!cpp || !horiz_pixels)
		return 0;
	return DIV_ROUND_UP(pri_val * 64, horiz_pixels * cpp) + 2;
}

static void wm_g4x_raw_plane_compute(const struct wm_in *in, int plane,
				     struct wm_g4x_raw raw[G4X_NUM_LEVELS])
{
	int level;

	if (!wm_plane_visible(in, plane)) {
		for (level = 0; level < g_wm.num_levels; level++) {
			raw[level].plane[plane] = 0;
			if (plane == WM_PRIMARY)
				raw[level].fbc = 0;
		}
		return;
	}

	for (level = 0; level < g_wm.num_levels; level++) {
		uint32_t wm, max_wm;

		wm = wm_g4x_compute_wm(in, plane, level);
		max_wm = wm_g4x_plane_fifo_size(plane, level);
		if (wm > max_wm)
			break;

		raw[level].plane[plane] = (uint16_t)wm;

		if (plane != WM_PRIMARY || level == G4X_LEVEL_NORMAL)
			continue;

		wm = wm_compute_fbc_wm(in, raw[level].plane[plane]);
		max_wm = wm_g4x_fbc_fifo_size(level);

		/* The FBC watermark is optional: its use can always be
		 * turned off instead. */
		if (wm > max_wm)
			wm = WM_INVALID;
		raw[level].fbc = (uint16_t)wm;
	}

	/* the rest cannot be used */
	for (; level < g_wm.num_levels; level++) {
		raw[level].plane[plane] = WM_INVALID;
		if (plane == WM_PRIMARY)
			raw[level].fbc = WM_INVALID;
	}
}

static int wm_g4x_raw_crtc_valid(const struct wm_g4x_raw raw[G4X_NUM_LEVELS], int level)
{
	if (level >= g_wm.num_levels)
		return 0;

	return raw[level].plane[WM_PRIMARY] <= wm_g4x_plane_fifo_size(WM_PRIMARY, level) &&
	       raw[level].plane[WM_SPRITE0] <= wm_g4x_plane_fifo_size(WM_SPRITE0, level) &&
	       raw[level].plane[WM_CURSOR] <= wm_g4x_plane_fifo_size(WM_CURSOR, level);
}

/* Mark every level from `level' on as unusable. */
static void wm_g4x_invalidate(struct wm_g4x_state *st, int level)
{
	if (level <= G4X_LEVEL_NORMAL) {
		st->wm[WM_PRIMARY] = WM_INVALID;
		st->wm[WM_SPRITE0] = WM_INVALID;
		st->wm[WM_CURSOR] = WM_INVALID;
	}

	if (level <= G4X_LEVEL_SR) {
		st->cxsr = 0;
		st->sr.cursor = WM_INVALID;
		st->sr.plane = WM_INVALID;
		st->sr.fbc = WM_INVALID;
	}

	if (level <= G4X_LEVEL_HPLL) {
		st->hpll_en = 0;
		st->hpll.cursor = WM_INVALID;
		st->hpll.plane = WM_INVALID;
		st->hpll.fbc = WM_INVALID;
	}
}

static int wm_g4x_compute_fbc_en(const struct wm_g4x_state *st, int level)
{
	if (level < G4X_LEVEL_SR)
		return 0;

	if (level >= G4X_LEVEL_SR && st->sr.fbc > wm_g4x_fbc_fifo_size(G4X_LEVEL_SR))
		return 0;

	if (level >= G4X_LEVEL_HPLL && st->hpll.fbc > wm_g4x_fbc_fifo_size(G4X_LEVEL_HPLL))
		return 0;

	return 1;
}

static void wm_g4x_compute_pipe(struct lg_display *d, int pipe, struct wm_g4x_state *st)
{
	struct wm_g4x_raw raw[G4X_NUM_LEVELS];
	struct wm_in in;
	int level, plane;
	int primary_only;

	wm_get_in(d, pipe, &in);
	mm_memset(raw, 0, sizeof(raw));
	mm_memset(st, 0, sizeof(*st));

	wm_g4x_raw_plane_compute(&in, WM_PRIMARY, raw);
	wm_g4x_raw_plane_compute(&in, WM_SPRITE0, raw);
	wm_g4x_raw_plane_compute(&in, WM_CURSOR, raw);

	i915_dbg("[drm] i915: wm: pipe %c primary watermarks: normal=%u, SR=%u, HPLL=%u\n",
		 wm_pipe_name(pipe), raw[0].plane[WM_PRIMARY], raw[1].plane[WM_PRIMARY],
		 raw[2].plane[WM_PRIMARY]);

	/* the primary is the only plane besides the cursor */
	primary_only = wm_plane_visible(&in, WM_PRIMARY);

	level = G4X_LEVEL_NORMAL;
	if (!wm_g4x_raw_crtc_valid(raw, level))
		goto out;

	for (plane = 0; plane < WM_NUM_PLANES; plane++)
		st->wm[plane] = raw[level].plane[plane];

	level = G4X_LEVEL_SR;
	if (!wm_g4x_raw_crtc_valid(raw, level))
		goto out;

	st->sr.plane = raw[level].plane[WM_PRIMARY];
	st->sr.cursor = raw[level].plane[WM_CURSOR];
	st->sr.fbc = raw[level].fbc;

	st->cxsr = primary_only;

	level = G4X_LEVEL_HPLL;
	if (!wm_g4x_raw_crtc_valid(raw, level))
		goto out;

	st->hpll.plane = raw[level].plane[WM_PRIMARY];
	st->hpll.cursor = raw[level].plane[WM_CURSOR];
	st->hpll.fbc = raw[level].fbc;

	st->hpll_en = st->cxsr;

	level++;

out:
	if (level == G4X_LEVEL_NORMAL) {
		/* The mode check should have refused this: the FIFO cannot
		 * cover the latency.  Run with the largest watermarks. */
		kprintf("[drm] i915: watermarks: pipe %c exceeds the display FIFO\n",
			wm_pipe_name(pipe));
		for (plane = 0; plane < WM_NUM_PLANES; plane++)
			st->wm[plane] = (uint16_t)wm_min_u32(
				raw[level].plane[plane],
				wm_g4x_plane_fifo_size(plane, G4X_LEVEL_NORMAL));
		level = G4X_LEVEL_SR;
	}

	/* the higher levels are unusable */
	wm_g4x_invalidate(st, level);

	/* Rather than give up the SR/HPLL levels, give up the FBC
	 * watermarks; level - 1 is the highest usable one. */
	st->fbc_en = wm_g4x_compute_fbc_en(st, level - 1);
}

static void wm_g4x_write_values(struct lg_display *d, const struct wm_g4x_values *wm)
{
	lg_wr(d, DSPFW1, FW_WM(wm->sr.plane, SR) | FW_WM(wm->pipe[1][WM_CURSOR], CURSORB) |
				 FW_WM(wm->pipe[1][WM_PRIMARY], PLANEB) |
				 FW_WM(wm->pipe[0][WM_PRIMARY], PLANEA));
	lg_wr(d, DSPFW2, (wm->fbc_en ? DSPFW_FBC_SR_EN : 0) | FW_WM(wm->sr.fbc, FBC_SR) |
				 FW_WM(wm->hpll.fbc, FBC_HPLL_SR) |
				 FW_WM(wm->pipe[1][WM_SPRITE0], SPRITEB) |
				 FW_WM(wm->pipe[0][WM_CURSOR], CURSORA) |
				 FW_WM(wm->pipe[0][WM_SPRITE0], SPRITEA));
	lg_wr(d, DSPFW3, (wm->hpll_en ? DSPFW_HPLL_SR_EN : 0) | FW_WM(wm->sr.cursor, CURSOR_SR) |
				 FW_WM(wm->hpll.cursor, HPLL_CURSOR) |
				 FW_WM(wm->hpll.plane, HPLL_SR));

	lg_posting_read(d, DSPFW1);
}

static void wm_g4x_read_values(struct lg_display *d, struct wm_g4x_values *wm)
{
	uint32_t tmp;

	tmp = lg_rd(d, DSPFW1);
	wm->sr.plane = (uint16_t)FW_WM_GET(tmp, SR);
	wm->pipe[1][WM_CURSOR] = (uint16_t)FW_WM_GET(tmp, CURSORB);
	wm->pipe[1][WM_PRIMARY] = (uint16_t)FW_WM_GET(tmp, PLANEB);
	wm->pipe[0][WM_PRIMARY] = (uint16_t)FW_WM_GET(tmp, PLANEA);

	tmp = lg_rd(d, DSPFW2);
	wm->fbc_en = !!(tmp & DSPFW_FBC_SR_EN);
	wm->sr.fbc = (uint16_t)FW_WM_GET(tmp, FBC_SR);
	wm->hpll.fbc = (uint16_t)FW_WM_GET(tmp, FBC_HPLL_SR);
	wm->pipe[1][WM_SPRITE0] = (uint16_t)FW_WM_GET(tmp, SPRITEB);
	wm->pipe[0][WM_CURSOR] = (uint16_t)FW_WM_GET(tmp, CURSORA);
	wm->pipe[0][WM_SPRITE0] = (uint16_t)FW_WM_GET(tmp, SPRITEA);

	tmp = lg_rd(d, DSPFW3);
	wm->hpll_en = !!(tmp & DSPFW_HPLL_SR_EN);
	wm->sr.cursor = (uint16_t)FW_WM_GET(tmp, CURSOR_SR);
	wm->hpll.cursor = (uint16_t)FW_WM_GET(tmp, HPLL_CURSOR);
	wm->hpll.plane = (uint16_t)FW_WM_GET(tmp, HPLL_SR);
}

static void wm_g4x_update(struct lg_display *d)
{
	struct wm_g4x_state st[LG_MAX_PIPES];
	struct wm_g4x_values new_wm;
	struct wm_g4x_values *old_wm = &g_wm.g4x;
	int pipe, num_active_pipes = 0;

	for (pipe = 0; pipe < d->num_pipes; pipe++)
		wm_g4x_compute_pipe(d, pipe, &st[pipe]);

	/* merge: self refresh only with a single active pipe that allows it */
	mm_memset(&new_wm, 0, sizeof(new_wm));
	new_wm.cxsr = 1;
	new_wm.hpll_en = 1;
	new_wm.fbc_en = 1;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		if (!d->pipes[pipe].active)
			continue;

		if (!st[pipe].cxsr)
			new_wm.cxsr = 0;
		if (!st[pipe].hpll_en)
			new_wm.hpll_en = 0;
		if (!st[pipe].fbc_en)
			new_wm.fbc_en = 0;

		num_active_pipes++;
	}

	if (num_active_pipes != 1) {
		new_wm.cxsr = 0;
		new_wm.hpll_en = 0;
		new_wm.fbc_en = 0;
	}

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		int plane;

		for (plane = 0; plane < WM_NUM_PLANES; plane++)
			new_wm.pipe[pipe][plane] = st[pipe].wm[plane];
		if (d->pipes[pipe].active && new_wm.cxsr)
			new_wm.sr = st[pipe].sr;
		if (d->pipes[pipe].active && new_wm.hpll_en)
			new_wm.hpll = st[pipe].hpll;
	}

	if (g_wm.g4x_valid && wm_same(old_wm, &new_wm, sizeof(new_wm)))
		return;

	if (is_disabling(old_wm->cxsr, new_wm.cxsr, 1))
		wm_set_cxsr_hw(d, 0);

	wm_g4x_write_values(d, &new_wm);

	if (is_enabling(old_wm->cxsr, new_wm.cxsr, 1))
		wm_set_cxsr_hw(d, 1);

	i915_dbg("[drm] i915: wm: G4X A=%u/%u B=%u/%u SR=%u/%u/%u HPLL=%u/%u/%u cxsr=%u hpll=%u fbc=%u\n",
		 new_wm.pipe[0][WM_PRIMARY], new_wm.pipe[0][WM_CURSOR], new_wm.pipe[1][WM_PRIMARY],
		 new_wm.pipe[1][WM_CURSOR], new_wm.sr.plane, new_wm.sr.cursor, new_wm.sr.fbc,
		 new_wm.hpll.plane, new_wm.hpll.cursor, new_wm.hpll.fbc, new_wm.cxsr,
		 new_wm.hpll_en, new_wm.fbc_en);

	*old_wm = new_wm;
	g_wm.g4x_valid = 1;
}

/* ---- Valleyview / Cherryview -------------------------------------------------------- */

static void wm_vlv_setup_latency(struct lg_display *d)
{
	/* in us */
	g_wm.pri_latency[VLV_LEVEL_PM2] = 3;
	g_wm.num_levels = VLV_LEVEL_PM2 + 1;

	if (d->is_chv) {
		g_wm.pri_latency[VLV_LEVEL_PM5] = 12;
		g_wm.pri_latency[VLV_LEVEL_DDR_DVFS] = 33;
		g_wm.num_levels = VLV_LEVEL_DDR_DVFS + 1;
	}
}

/* Poke the Punit's DDR frequency request and wait for its ack. */
static int wm_chv_ddr_wait_ack(struct lg_display *d)
{
	uint32_t waited = 0, val;

	for (;;) {
		val = lg_vlv_punit_read(d, PUNIT_REG_DDR_SETUP2);
		if (!(val & FORCE_DDR_FREQ_REQ_ACK))
			return 0;
		if (waited >= 3000)
			return -ETIMEDOUT;
		lg_udelay(500);
		waited += 500;
	}
}

static void wm_chv_set_memory_dvfs(struct lg_display *d, int enable)
{
	uint32_t val;

	val = lg_vlv_punit_read(d, PUNIT_REG_DDR_SETUP2);
	if (enable)
		val &= ~FORCE_DDR_HIGH_FREQ;
	else
		val |= FORCE_DDR_HIGH_FREQ;
	val &= ~FORCE_DDR_LOW_FREQ;
	val |= FORCE_DDR_FREQ_REQ_ACK;
	lg_vlv_punit_write(d, PUNIT_REG_DDR_SETUP2, val);

	if (wm_chv_ddr_wait_ack(d))
		kprintf("[drm] i915: timed out waiting for Punit DDR DVFS request\n");
}

static void wm_chv_set_memory_pm5(struct lg_display *d, int enable)
{
	uint32_t val;

	val = lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM);
	if (enable)
		val |= DSP_MAXFIFO_PM5_ENABLE;
	else
		val &= ~DSP_MAXFIFO_PM5_ENABLE;
	lg_vlv_punit_write(d, PUNIT_REG_DSPSSPM, val);
}

static uint16_t wm_vlv_compute_wm_level(const struct wm_in *in, int plane, int level)
{
	uint32_t wm;

	if (g_wm.pri_latency[level] == 0)
		return WM_INVALID;

	if (!wm_plane_visible(in, plane))
		return 0;

	if (plane == WM_CURSOR) {
		/* The formula gives values too big for the cursor FIFO,
		 * which would leave the cursor unusable: hardcode it. */
		wm = 63;
	} else {
		wm = wm_method2(in->pixel_rate, in->htotal, in->width, in->cpp,
				(uint32_t)g_wm.pri_latency[level] * 10);
		wm = DIV_ROUND_UP(wm, 64);
	}

	return (uint16_t)wm_min_u32(wm, WM_INVALID);
}

static void wm_vlv_raw_plane_compute(const struct wm_in *in, int plane,
				     uint16_t raw[VLV_NUM_LEVELS][WM_NUM_PLANES])
{
	int level;

	if (!wm_plane_visible(in, plane)) {
		for (level = 0; level < g_wm.num_levels; level++)
			raw[level][plane] = 0;
		return;
	}

	for (level = 0; level < g_wm.num_levels; level++) {
		uint32_t wm = wm_vlv_compute_wm_level(in, plane, level);
		uint32_t max_wm = plane == WM_CURSOR ? 63 : 511;

		if (wm > max_wm)
			break;
		raw[level][plane] = (uint16_t)wm;
	}

	/* the higher levels cannot be used */
	for (; level < g_wm.num_levels; level++)
		raw[level][plane] = WM_INVALID;
}

/*
 * Split a pipe's 511-cacheline FIFO between its primary and sprite planes
 * in proportion to their PM2 watermarks, the remainder spread evenly; the
 * cursor has its own 63.  Only primaries are used here, so the primary
 * gets everything.  -EINVAL when the planes need more than there is.
 */
static int wm_vlv_compute_fifo(const uint16_t raw[WM_NUM_PLANES], uint32_t active_planes,
			       struct wm_vlv_fifo *fifo)
{
	const int fifo_size = 511;
	int num_active_planes = 0;
	int fifo_extra, fifo_left = fifo_size;
	int sprite0_fifo_extra = 0;
	uint32_t total_rate;
	int plane;

	for (plane = 0; plane < WM_CURSOR; plane++)
		if (active_planes & (1u << plane))
			num_active_planes++;

	/* Enabling sprite 0 after sprite 1 underruns unless sprite 0
	 * already has some FIFO space: give it a cacheline whenever
	 * sprite 1 is on. */
	if ((active_planes & ((1u << WM_SPRITE0) | (1u << WM_SPRITE1))) == (1u << WM_SPRITE1))
		sprite0_fifo_extra = 1;

	total_rate = (uint32_t)raw[WM_PRIMARY] + raw[WM_SPRITE0] + raw[WM_SPRITE1] +
		     (uint32_t)sprite0_fifo_extra;

	if (total_rate > (uint32_t)fifo_size) {
		/* everything to the primary, the plane's levels will
		 * show it cannot be served */
		mm_memset(fifo, 0, sizeof(*fifo));
		fifo->plane[WM_PRIMARY] = (uint16_t)fifo_size;
		fifo->plane[WM_CURSOR] = 63;
		return -EINVAL;
	}

	if (total_rate == 0)
		total_rate = 1;

	for (plane = 0; plane < WM_CURSOR; plane++) {
		uint32_t rate;

		if (!(active_planes & (1u << plane))) {
			fifo->plane[plane] = 0;
			continue;
		}

		rate = raw[plane];
		fifo->plane[plane] = (uint16_t)((uint32_t)fifo_size * rate / total_rate);
		fifo_left -= fifo->plane[plane];
	}

	fifo->plane[WM_SPRITE0] = (uint16_t)(fifo->plane[WM_SPRITE0] + sprite0_fifo_extra);
	fifo_left -= sprite0_fifo_extra;

	fifo->plane[WM_CURSOR] = 63;

	fifo_extra = DIV_ROUND_UP(fifo_left, num_active_planes ? num_active_planes : 1);

	/* spread the remainder evenly */
	for (plane = 0; plane < WM_CURSOR; plane++) {
		int plane_extra;

		if (fifo_left == 0)
			break;

		if (!(active_planes & (1u << plane)))
			continue;

		plane_extra = fifo_extra < fifo_left ? fifo_extra : fifo_left;
		fifo->plane[plane] = (uint16_t)(fifo->plane[plane] + plane_extra);
		fifo_left -= plane_extra;
	}

	/* no plane active: all of it to the first */
	if (active_planes == 0)
		fifo->plane[WM_PRIMARY] = (uint16_t)fifo_left;

	return 0;
}

static uint16_t wm_vlv_invert(uint16_t wm, uint16_t fifo_size)
{
	if (wm > fifo_size)
		return WM_INVALID;
	return (uint16_t)(fifo_size - wm);
}

static void wm_vlv_invalidate(struct wm_vlv_state *st, int level)
{
	int plane;

	for (; level < g_wm.num_levels; level++) {
		for (plane = 0; plane < WM_NUM_PLANES; plane++)
			st->wm[level][plane] = WM_INVALID;
		st->sr[level].cursor = WM_INVALID;
		st->sr[level].plane = WM_INVALID;
	}
}

static int wm_vlv_raw_crtc_valid(const uint16_t raw[WM_NUM_PLANES],
				 const struct wm_vlv_fifo *fifo)
{
	int plane;

	for (plane = 0; plane < WM_NUM_PLANES; plane++)
		if (raw[plane] > fifo->plane[plane])
			return 0;
	return 1;
}

static void wm_vlv_compute_pipe(struct lg_display *d, int pipe, struct wm_vlv_state *st,
				struct wm_vlv_fifo *fifo)
{
	uint16_t raw[VLV_NUM_LEVELS][WM_NUM_PLANES];
	const uint16_t sr_fifo_size = (uint16_t)(d->num_pipes * 512 - 1);
	uint32_t active_planes = 0;
	int num_active_planes;
	struct wm_in in;
	int level, plane;

	wm_get_in(d, pipe, &in);
	mm_memset(raw, 0, sizeof(raw));
	mm_memset(st, 0, sizeof(*st));

	for (plane = 0; plane < WM_NUM_PLANES; plane++)
		wm_vlv_raw_plane_compute(&in, plane, raw);

	if (wm_plane_visible(&in, WM_PRIMARY))
		active_planes |= 1u << WM_PRIMARY;
	num_active_planes = active_planes ? 1 : 0;

	i915_dbg("[drm] i915: wm: pipe %c primary watermarks: PM2=%u, PM5=%u, DDR DVFS=%u\n",
		 wm_pipe_name(pipe), raw[0][WM_PRIMARY], raw[1][WM_PRIMARY], raw[2][WM_PRIMARY]);

	if (wm_vlv_compute_fifo(raw[VLV_LEVEL_PM2], active_planes, fifo))
		i915_dbg("[drm] i915: wm: pipe %c planes exceed the FIFO\n", wm_pipe_name(pipe));

	st->num_levels = g_wm.num_levels;
	/* CxSR with no primary or sprite plane enabled can wedge the
	 * pipe: allow it only with exactly one of them. */
	st->cxsr = pipe != 2 && num_active_planes == 1;

	for (level = 0; level < st->num_levels; level++) {
		uint16_t pri_spr;

		if (!wm_vlv_raw_crtc_valid(raw[level], fifo))
			break;

		for (plane = 0; plane < WM_NUM_PLANES; plane++)
			st->wm[level][plane] = wm_vlv_invert(raw[level][plane], fifo->plane[plane]);

		pri_spr = raw[level][WM_PRIMARY];
		if (raw[level][WM_SPRITE0] > pri_spr)
			pri_spr = raw[level][WM_SPRITE0];
		if (raw[level][WM_SPRITE1] > pri_spr)
			pri_spr = raw[level][WM_SPRITE1];
		st->sr[level].plane = wm_vlv_invert(pri_spr, sr_fifo_size);
		st->sr[level].cursor = wm_vlv_invert(raw[level][WM_CURSOR], 63);
	}

	if (level == 0) {
		/* The mode check should have refused this.  Run with the
		 * most urgent watermarks and no self refresh. */
		kprintf("[drm] i915: watermarks: pipe %c exceeds the display FIFO\n",
			wm_pipe_name(pipe));
		for (plane = 0; plane < WM_NUM_PLANES; plane++)
			st->wm[0][plane] = 0;
		st->sr[0].plane = 0;
		st->sr[0].cursor = 0;
		st->cxsr = 0;
		level = 1;
	}

	/* only the levels that can be used */
	st->num_levels = level;
	wm_vlv_invalidate(st, level);
}

static void wm_vlv_write_values(struct lg_display *d, const struct wm_vlv_values *wm)
{
	int pipe;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		lg_wr(d, VLV_DDL(pipe),
		      ((uint32_t)wm->ddl[pipe][WM_CURSOR] << DDL_CURSOR_SHIFT) |
			      ((uint32_t)wm->ddl[pipe][WM_SPRITE1] << DDL_SPRITE_SHIFT(1)) |
			      ((uint32_t)wm->ddl[pipe][WM_SPRITE0] << DDL_SPRITE_SHIFT(0)) |
			      ((uint32_t)wm->ddl[pipe][WM_PRIMARY] << DDL_PLANE_SHIFT));
	}

	/* Zero the unused WM1 watermarks and all the high bits, so that no
	 * out-of-range value is in the registers while they change. */
	lg_wr(d, DSPHOWM, 0);
	lg_wr(d, DSPHOWM1, 0);
	lg_wr(d, DSPFW4, 0);
	lg_wr(d, DSPFW5, 0);
	lg_wr(d, DSPFW6, 0);

	lg_wr(d, DSPFW1, FW_WM(wm->sr.plane, SR) | FW_WM(wm->pipe[1][WM_CURSOR], CURSORB) |
				 FW_WM_VLV(wm->pipe[1][WM_PRIMARY], PLANEB) |
				 FW_WM_VLV(wm->pipe[0][WM_PRIMARY], PLANEA));
	lg_wr(d, DSPFW2, FW_WM_VLV(wm->pipe[0][WM_SPRITE1], SPRITEB) |
				 FW_WM(wm->pipe[0][WM_CURSOR], CURSORA) |
				 FW_WM_VLV(wm->pipe[0][WM_SPRITE0], SPRITEA));
	lg_wr(d, DSPFW3, FW_WM(wm->sr.cursor, CURSOR_SR));

	if (d->is_chv) {
		lg_wr(d, DSPFW7_CHV, FW_WM_VLV(wm->pipe[1][WM_SPRITE1], SPRITED) |
					     FW_WM_VLV(wm->pipe[1][WM_SPRITE0], SPRITEC));
		lg_wr(d, DSPFW8_CHV, FW_WM_VLV(wm->pipe[2][WM_SPRITE1], SPRITEF) |
					     FW_WM_VLV(wm->pipe[2][WM_SPRITE0], SPRITEE));
		lg_wr(d, DSPFW9_CHV, FW_WM_VLV(wm->pipe[2][WM_PRIMARY], PLANEC) |
					     FW_WM(wm->pipe[2][WM_CURSOR], CURSORC));
		lg_wr(d, DSPHOWM,
		      FW_WM(wm->sr.plane >> 9, SR_HI) |
			      FW_WM(wm->pipe[2][WM_SPRITE1] >> 8, SPRITEF_HI) |
			      FW_WM(wm->pipe[2][WM_SPRITE0] >> 8, SPRITEE_HI) |
			      FW_WM(wm->pipe[2][WM_PRIMARY] >> 8, PLANEC_HI) |
			      FW_WM(wm->pipe[1][WM_SPRITE1] >> 8, SPRITED_HI) |
			      FW_WM(wm->pipe[1][WM_SPRITE0] >> 8, SPRITEC_HI) |
			      FW_WM(wm->pipe[1][WM_PRIMARY] >> 8, PLANEB_HI) |
			      FW_WM(wm->pipe[0][WM_SPRITE1] >> 8, SPRITEB_HI) |
			      FW_WM(wm->pipe[0][WM_SPRITE0] >> 8, SPRITEA_HI) |
			      FW_WM(wm->pipe[0][WM_PRIMARY] >> 8, PLANEA_HI));
	} else {
		lg_wr(d, DSPFW7, FW_WM_VLV(wm->pipe[1][WM_SPRITE1], SPRITED) |
					 FW_WM_VLV(wm->pipe[1][WM_SPRITE0], SPRITEC));
		lg_wr(d, DSPHOWM,
		      FW_WM(wm->sr.plane >> 9, SR_HI) |
			      FW_WM(wm->pipe[1][WM_SPRITE1] >> 8, SPRITED_HI) |
			      FW_WM(wm->pipe[1][WM_SPRITE0] >> 8, SPRITEC_HI) |
			      FW_WM(wm->pipe[1][WM_PRIMARY] >> 8, PLANEB_HI) |
			      FW_WM(wm->pipe[0][WM_SPRITE1] >> 8, SPRITEB_HI) |
			      FW_WM(wm->pipe[0][WM_SPRITE0] >> 8, SPRITEA_HI) |
			      FW_WM(wm->pipe[0][WM_PRIMARY] >> 8, PLANEA_HI));
	}

	lg_posting_read(d, DSPFW1);
}

static void wm_vlv_read_values(struct lg_display *d, struct wm_vlv_values *wm)
{
	int pipe;
	uint32_t tmp;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		const uint32_t m = DDL_PRECISION_HIGH | DRAIN_LATENCY_MASK;

		tmp = lg_rd(d, VLV_DDL(pipe));
		wm->ddl[pipe][WM_PRIMARY] = (uint8_t)((tmp >> DDL_PLANE_SHIFT) & m);
		wm->ddl[pipe][WM_CURSOR] = (uint8_t)((tmp >> DDL_CURSOR_SHIFT) & m);
		wm->ddl[pipe][WM_SPRITE0] = (uint8_t)((tmp >> DDL_SPRITE_SHIFT(0)) & m);
		wm->ddl[pipe][WM_SPRITE1] = (uint8_t)((tmp >> DDL_SPRITE_SHIFT(1)) & m);
	}

	tmp = lg_rd(d, DSPFW1);
	wm->sr.plane = (uint16_t)FW_WM_GET(tmp, SR);
	wm->pipe[1][WM_CURSOR] = (uint16_t)FW_WM_GET(tmp, CURSORB);
	wm->pipe[1][WM_PRIMARY] = (uint16_t)FW_WM_GET_VLV(tmp, PLANEB);
	wm->pipe[0][WM_PRIMARY] = (uint16_t)FW_WM_GET_VLV(tmp, PLANEA);

	tmp = lg_rd(d, DSPFW2);
	wm->pipe[0][WM_SPRITE1] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITEB);
	wm->pipe[0][WM_CURSOR] = (uint16_t)FW_WM_GET(tmp, CURSORA);
	wm->pipe[0][WM_SPRITE0] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITEA);

	tmp = lg_rd(d, DSPFW3);
	wm->sr.cursor = (uint16_t)FW_WM_GET(tmp, CURSOR_SR);

	if (d->is_chv) {
		tmp = lg_rd(d, DSPFW7_CHV);
		wm->pipe[1][WM_SPRITE1] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITED);
		wm->pipe[1][WM_SPRITE0] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITEC);

		tmp = lg_rd(d, DSPFW8_CHV);
		wm->pipe[2][WM_SPRITE1] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITEF);
		wm->pipe[2][WM_SPRITE0] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITEE);

		tmp = lg_rd(d, DSPFW9_CHV);
		wm->pipe[2][WM_PRIMARY] = (uint16_t)FW_WM_GET_VLV(tmp, PLANEC);
		wm->pipe[2][WM_CURSOR] = (uint16_t)FW_WM_GET(tmp, CURSORC);

		tmp = lg_rd(d, DSPHOWM);
		wm->sr.plane |= (uint16_t)(FW_WM_GET(tmp, SR_HI) << 9);
		wm->pipe[2][WM_SPRITE1] |= (uint16_t)(FW_WM_GET(tmp, SPRITEF_HI) << 8);
		wm->pipe[2][WM_SPRITE0] |= (uint16_t)(FW_WM_GET(tmp, SPRITEE_HI) << 8);
		wm->pipe[2][WM_PRIMARY] |= (uint16_t)(FW_WM_GET(tmp, PLANEC_HI) << 8);
		wm->pipe[1][WM_SPRITE1] |= (uint16_t)(FW_WM_GET(tmp, SPRITED_HI) << 8);
		wm->pipe[1][WM_SPRITE0] |= (uint16_t)(FW_WM_GET(tmp, SPRITEC_HI) << 8);
		wm->pipe[1][WM_PRIMARY] |= (uint16_t)(FW_WM_GET(tmp, PLANEB_HI) << 8);
		wm->pipe[0][WM_SPRITE1] |= (uint16_t)(FW_WM_GET(tmp, SPRITEB_HI) << 8);
		wm->pipe[0][WM_SPRITE0] |= (uint16_t)(FW_WM_GET(tmp, SPRITEA_HI) << 8);
		wm->pipe[0][WM_PRIMARY] |= (uint16_t)(FW_WM_GET(tmp, PLANEA_HI) << 8);
	} else {
		tmp = lg_rd(d, DSPFW7);
		wm->pipe[1][WM_SPRITE1] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITED);
		wm->pipe[1][WM_SPRITE0] = (uint16_t)FW_WM_GET_VLV(tmp, SPRITEC);

		tmp = lg_rd(d, DSPHOWM);
		wm->sr.plane |= (uint16_t)(FW_WM_GET(tmp, SR_HI) << 9);
		wm->pipe[1][WM_SPRITE1] |= (uint16_t)(FW_WM_GET(tmp, SPRITED_HI) << 8);
		wm->pipe[1][WM_SPRITE0] |= (uint16_t)(FW_WM_GET(tmp, SPRITEC_HI) << 8);
		wm->pipe[1][WM_PRIMARY] |= (uint16_t)(FW_WM_GET(tmp, PLANEB_HI) << 8);
		wm->pipe[0][WM_SPRITE1] |= (uint16_t)(FW_WM_GET(tmp, SPRITEB_HI) << 8);
		wm->pipe[0][WM_SPRITE0] |= (uint16_t)(FW_WM_GET(tmp, SPRITEA_HI) << 8);
		wm->pipe[0][WM_PRIMARY] |= (uint16_t)(FW_WM_GET(tmp, PLANEA_HI) << 8);
	}
}

#define VLV_FIFO_START(dsparb, dsparb2, lo_shift, hi_shift)                    \
	((((dsparb) >> (lo_shift)) & 0xff) | ((((dsparb2) >> (hi_shift)) & 0x1) << 8))

/* The split the firmware left in DSPARB. */
static void wm_vlv_get_fifo_size(struct lg_display *d, int pipe, struct wm_vlv_fifo *fifo)
{
	uint32_t dsparb, dsparb2, dsparb3;
	uint32_t sprite0_start, sprite1_start;

	switch (pipe) {
	case 0:
		dsparb = lg_rd(d, DSPARB);
		dsparb2 = lg_rd(d, DSPARB2);
		sprite0_start = VLV_FIFO_START(dsparb, dsparb2, 0, 0);
		sprite1_start = VLV_FIFO_START(dsparb, dsparb2, 8, 4);
		break;
	case 1:
		dsparb = lg_rd(d, DSPARB);
		dsparb2 = lg_rd(d, DSPARB2);
		sprite0_start = VLV_FIFO_START(dsparb, dsparb2, 16, 8);
		sprite1_start = VLV_FIFO_START(dsparb, dsparb2, 24, 12);
		break;
	case 2:
		dsparb2 = lg_rd(d, DSPARB2);
		dsparb3 = lg_rd(d, DSPARB3);
		sprite0_start = VLV_FIFO_START(dsparb3, dsparb2, 0, 16);
		sprite1_start = VLV_FIFO_START(dsparb3, dsparb2, 8, 20);
		break;
	default:
		return;
	}

	fifo->plane[WM_PRIMARY] = (uint16_t)sprite0_start;
	fifo->plane[WM_SPRITE0] = (uint16_t)(sprite1_start - sprite0_start);
	fifo->plane[WM_SPRITE1] = (uint16_t)(511 - sprite1_start);
	fifo->plane[WM_CURSOR] = 63;
}

/* Program a pipe's FIFO split: where its sprites start. */
static void wm_vlv_write_fifo(struct lg_display *d, int pipe, const struct wm_vlv_fifo *fifo)
{
	uint32_t sprite0_start, sprite1_start, fifo_size;
	uint32_t dsparb, dsparb2, dsparb3;

	sprite0_start = fifo->plane[WM_PRIMARY];
	sprite1_start = fifo->plane[WM_SPRITE0] + sprite0_start;
	fifo_size = fifo->plane[WM_SPRITE1] + sprite1_start;

	i915_dbg("[drm] i915: wm: pipe %c FIFO split %u/%u/%u\n", wm_pipe_name(pipe),
		 sprite0_start, sprite1_start, fifo_size);

	switch (pipe) {
	case 0:
		dsparb = lg_rd(d, DSPARB);
		dsparb2 = lg_rd(d, DSPARB2);

		dsparb &= ~(VLV_FIFO(SPRITEA, 0xff) | VLV_FIFO(SPRITEB, 0xff));
		dsparb |= (VLV_FIFO(SPRITEA, sprite0_start) | VLV_FIFO(SPRITEB, sprite1_start));

		dsparb2 &= ~(VLV_FIFO(SPRITEA_HI, 0x1) | VLV_FIFO(SPRITEB_HI, 0x1));
		dsparb2 |= (VLV_FIFO(SPRITEA_HI, sprite0_start >> 8) |
			    VLV_FIFO(SPRITEB_HI, sprite1_start >> 8));

		lg_wr(d, DSPARB, dsparb);
		lg_wr(d, DSPARB2, dsparb2);
		break;
	case 1:
		dsparb = lg_rd(d, DSPARB);
		dsparb2 = lg_rd(d, DSPARB2);

		dsparb &= ~(VLV_FIFO(SPRITEC, 0xff) | VLV_FIFO(SPRITED, 0xff));
		dsparb |= (VLV_FIFO(SPRITEC, sprite0_start) | VLV_FIFO(SPRITED, sprite1_start));

		dsparb2 &= ~(VLV_FIFO(SPRITEC_HI, 0xff) | VLV_FIFO(SPRITED_HI, 0xff));
		dsparb2 |= (VLV_FIFO(SPRITEC_HI, sprite0_start >> 8) |
			    VLV_FIFO(SPRITED_HI, sprite1_start >> 8));

		lg_wr(d, DSPARB, dsparb);
		lg_wr(d, DSPARB2, dsparb2);
		break;
	case 2:
		dsparb3 = lg_rd(d, DSPARB3);
		dsparb2 = lg_rd(d, DSPARB2);

		dsparb3 &= ~(VLV_FIFO(SPRITEE, 0xff) | VLV_FIFO(SPRITEF, 0xff));
		dsparb3 |= (VLV_FIFO(SPRITEE, sprite0_start) | VLV_FIFO(SPRITEF, sprite1_start));

		dsparb2 &= ~(VLV_FIFO(SPRITEE_HI, 0xff) | VLV_FIFO(SPRITEF_HI, 0xff));
		dsparb2 |= (VLV_FIFO(SPRITEE_HI, sprite0_start >> 8) |
			    VLV_FIFO(SPRITEF_HI, sprite1_start >> 8));

		lg_wr(d, DSPARB3, dsparb3);
		lg_wr(d, DSPARB2, dsparb2);
		break;
	default:
		break;
	}

	lg_posting_read(d, DSPARB);
}

static void wm_vlv_update(struct lg_display *d)
{
	struct wm_vlv_state st[LG_MAX_PIPES];
	struct wm_vlv_fifo fifo[LG_MAX_PIPES];
	struct wm_vlv_values new_wm;
	struct wm_vlv_values *old_wm = &g_wm.vlv;
	int pipe, num_active_pipes = 0;
	int level;
	int fifo_changed = 0;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		mm_memset(&fifo[pipe], 0, sizeof(fifo[pipe]));
		wm_vlv_compute_pipe(d, pipe, &st[pipe], &fifo[pipe]);
		if (!g_wm.vlv_fifo_valid ||
		    !wm_same(&fifo[pipe], &g_wm.vlv_fifo[pipe], sizeof(fifo[pipe])))
			fifo_changed |= 1 << pipe;
	}

	/* A new FIFO split: no max FIFO mode while it changes (the split
	 * is not latched in it), and the split before the watermarks that
	 * are relative to it. */
	if (fifo_changed) {
		if (old_wm->cxsr)
			wm_set_cxsr(d, 0);
		for (pipe = 0; pipe < d->num_pipes; pipe++) {
			if (!(fifo_changed & (1 << pipe)))
				continue;
			wm_vlv_write_fifo(d, pipe, &fifo[pipe]);
			g_wm.vlv_fifo[pipe] = fifo[pipe];
		}
		g_wm.vlv_fifo_valid = 1;
	}

	/* merge: the deepest level all active pipes manage, self refresh
	 * only with a single pipe */
	mm_memset(&new_wm, 0, sizeof(new_wm));
	level = g_wm.num_levels - 1;
	new_wm.cxsr = 1;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		if (!d->pipes[pipe].active)
			continue;

		if (!st[pipe].cxsr)
			new_wm.cxsr = 0;

		num_active_pipes++;
		if (st[pipe].num_levels - 1 < level)
			level = st[pipe].num_levels - 1;
	}

	if (num_active_pipes != 1)
		new_wm.cxsr = 0;

	if (num_active_pipes > 1)
		level = VLV_LEVEL_PM2;

	new_wm.level = (uint16_t)level;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		int plane;

		for (plane = 0; plane < WM_NUM_PLANES; plane++) {
			new_wm.pipe[pipe][plane] = st[pipe].wm[level][plane];
			new_wm.ddl[pipe][plane] = (uint8_t)(DDL_PRECISION_HIGH | 2);
		}
		if (d->pipes[pipe].active && new_wm.cxsr)
			new_wm.sr = st[pipe].sr[level];
	}

	if (g_wm.vlv_valid && wm_same(old_wm, &new_wm, sizeof(new_wm)))
		return;

	if (d->is_chv && is_disabling(old_wm->level, new_wm.level, VLV_LEVEL_DDR_DVFS))
		wm_chv_set_memory_dvfs(d, 0);

	if (d->is_chv && is_disabling(old_wm->level, new_wm.level, VLV_LEVEL_PM5))
		wm_chv_set_memory_pm5(d, 0);

	if (is_disabling(old_wm->cxsr, new_wm.cxsr, 1))
		wm_set_cxsr_hw(d, 0);

	wm_vlv_write_values(d, &new_wm);

	if (is_enabling(old_wm->cxsr, new_wm.cxsr, 1))
		wm_set_cxsr_hw(d, 1);

	if (d->is_chv && is_enabling(old_wm->level, new_wm.level, VLV_LEVEL_PM5))
		wm_chv_set_memory_pm5(d, 1);

	if (d->is_chv && is_enabling(old_wm->level, new_wm.level, VLV_LEVEL_DDR_DVFS))
		wm_chv_set_memory_dvfs(d, 1);

	i915_dbg("[drm] i915: wm: VLV A=%u B=%u C=%u SR=%u/%u level=%u cxsr=%u\n",
		 new_wm.pipe[0][WM_PRIMARY], new_wm.pipe[1][WM_PRIMARY],
		 new_wm.pipe[2][WM_PRIMARY], new_wm.sr.plane, new_wm.sr.cursor, new_wm.level,
		 new_wm.cxsr);

	*old_wm = new_wm;
	g_wm.vlv_valid = 1;
}

/* What the firmware left: the watermarks, the FIFO split, self refresh
 * and (Cherryview) the PM5 and DDR DVFS state the Punit holds. */
static void wm_vlv_get_hw_state(struct lg_display *d)
{
	struct wm_vlv_values *wm = &g_wm.vlv;
	uint32_t val;
	int pipe;

	mm_memset(wm, 0, sizeof(*wm));
	wm_vlv_read_values(d, wm);

	wm->cxsr = !!(lg_rd(d, FW_BLC_SELF_VLV) & FW_CSPWRDWNEN);
	wm->level = VLV_LEVEL_PM2;

	if (d->is_chv) {
		val = lg_vlv_punit_read(d, PUNIT_REG_DSPSSPM);
		if (val & DSP_MAXFIFO_PM5_ENABLE)
			wm->level = VLV_LEVEL_PM5;

		/* With DDR DVFS off in the BIOS the Punit never acks a
		 * request.  Find out by setting just the request bit,
		 * leaving the frequency as it is. */
		val = lg_vlv_punit_read(d, PUNIT_REG_DDR_SETUP2);
		val |= FORCE_DDR_FREQ_REQ_ACK;
		lg_vlv_punit_write(d, PUNIT_REG_DDR_SETUP2, val);

		if (wm_chv_ddr_wait_ack(d)) {
			i915_dbg("[drm] i915: wm: Punit not acking DDR DVFS request, assuming DDR DVFS is disabled\n");
			g_wm.num_levels = VLV_LEVEL_PM5 + 1;
		} else {
			val = lg_vlv_punit_read(d, PUNIT_REG_DDR_SETUP2);
			if ((val & FORCE_DDR_HIGH_FREQ) == 0)
				wm->level = VLV_LEVEL_DDR_DVFS;
		}
	}

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		wm_vlv_get_fifo_size(d, pipe, &g_wm.vlv_fifo[pipe]);
		i915_dbg("[drm] i915: wm: initial pipe %c plane=%u cursor=%u fifo=%u/%u/%u\n",
			 wm_pipe_name(pipe), wm->pipe[pipe][WM_PRIMARY], wm->pipe[pipe][WM_CURSOR],
			 g_wm.vlv_fifo[pipe].plane[WM_PRIMARY], g_wm.vlv_fifo[pipe].plane[WM_SPRITE0],
			 g_wm.vlv_fifo[pipe].plane[WM_SPRITE1]);
	}
	g_wm.vlv_fifo_valid = 1;
	g_wm.vlv_valid = 1;

	i915_dbg("[drm] i915: wm: initial SR plane=%u, SR cursor=%u level=%u cxsr=%u\n",
		 wm->sr.plane, wm->sr.cursor, wm->level, wm->cxsr);
}

/* ---- Ironlake, Sandy Bridge, Ivy Bridge ---------------------------------------------- */

/* the method-1 and method-2 watermarks in 64-byte units, latency in 0.1 us */
static uint32_t wm_ilk_method1(uint32_t pixel_rate, uint32_t cpp, uint32_t latency)
{
	return DIV_ROUND_UP(wm_method1(pixel_rate, cpp, latency), 64) + 2;
}

static uint32_t wm_ilk_method2(uint32_t pixel_rate, uint32_t htotal, uint32_t width,
			       uint32_t cpp, uint32_t latency)
{
	return DIV_ROUND_UP(wm_method2(pixel_rate, htotal, width, cpp, latency), 64) + 2;
}

static uint32_t wm_ilk_compute_pri_wm(const struct wm_in *in, uint32_t mem_value, int is_lp)
{
	uint32_t method1, method2;

	if (mem_value == 0)
		return 0xffffffffu;

	if (!wm_plane_visible(in, WM_PRIMARY))
		return 0;

	method1 = wm_ilk_method1(in->pixel_rate, in->cpp, mem_value);
	if (!is_lp)
		return method1;

	method2 = wm_ilk_method2(in->pixel_rate, in->htotal, in->width, in->cpp, mem_value);
	return wm_min_u32(method1, method2);
}

/* No sprites are used: a sprite needs nothing, unless the level has no
 * latency at all. */
static uint32_t wm_ilk_compute_spr_wm(const struct wm_in *in, uint32_t mem_value)
{
	if (mem_value == 0)
		return 0xffffffffu;

	if (!wm_plane_visible(in, WM_SPRITE0))
		return 0;

	return 0;
}

static uint32_t wm_ilk_compute_cur_wm(const struct wm_in *in, uint32_t mem_value)
{
	if (mem_value == 0)
		return 0xffffffffu;

	if (!wm_plane_visible(in, WM_CURSOR))
		return 0;

	return wm_ilk_method2(in->pixel_rate, in->htotal, in->cur_w, 4, mem_value);
}

static uint32_t wm_ilk_display_fifo_size(struct lg_display *d)
{
	return d->ver >= 7 ? 768 : 512;
}

static uint32_t wm_ilk_plane_wm_reg_max(struct lg_display *d, int level, int is_sprite)
{
	if (d->ver >= 7)
		/* IVB primary/sprite plane watermarks */
		return level == 0 ? 127 : 1023;
	else if (!is_sprite)
		/* ILK/SNB primary plane watermarks */
		return level == 0 ? 127 : 511;
	else
		/* ILK/SNB sprite plane watermarks */
		return level == 0 ? 63 : 255;
}

static uint32_t wm_ilk_cursor_wm_reg_max(struct lg_display *d, int level)
{
	if (d->ver >= 7)
		return level == 0 ? 63 : 255;
	else
		return level == 0 ? 31 : 63;
}

static uint32_t wm_ilk_fbc_wm_reg_max(void)
{
	return 15;
}

/* The largest primary or sprite watermark the FIFO allows. */
static uint32_t wm_ilk_plane_wm_max(struct lg_display *d, int level,
				    const struct wm_ilk_config *config, int ddb_partitioning,
				    int is_sprite)
{
	uint32_t fifo_size = wm_ilk_display_fifo_size(d);

	/* without sprites the sprites get nothing */
	if (is_sprite && !config->sprites_enabled)
		return 0;

	if (level == 0 || config->num_pipes_active > 1) {
		fifo_size /= (uint32_t)d->num_pipes;

		/* On ILK/SNB the FIFO outside self refresh is only half
		 * of what self refresh gets. */
		if (d->ver < 7)
			fifo_size /= 2;
	}

	if (config->sprites_enabled) {
		/* level 0 is always computed with the 1:1 split */
		if (level > 0 && ddb_partitioning == ILK_DDB_PART_5_6) {
			if (is_sprite)
				fifo_size *= 5;
			fifo_size /= 6;
		} else {
			fifo_size /= 2;
		}
	}

	/* clamp to what the registers hold */
	return wm_min_u32(fifo_size, wm_ilk_plane_wm_reg_max(d, level, is_sprite));
}

static uint32_t wm_ilk_cursor_wm_max(struct lg_display *d, int level,
				     const struct wm_ilk_config *config)
{
	if (level > 0 && config->num_pipes_active > 1)
		return 64;

	return wm_ilk_cursor_wm_reg_max(d, level);
}

static void wm_ilk_compute_maximums(struct lg_display *d, int level,
				    const struct wm_ilk_config *config, int ddb_partitioning,
				    struct wm_ilk_max *max)
{
	max->pri = wm_ilk_plane_wm_max(d, level, config, ddb_partitioning, 0);
	max->spr = wm_ilk_plane_wm_max(d, level, config, ddb_partitioning, 1);
	max->cur = wm_ilk_cursor_wm_max(d, level, config);
	max->fbc = wm_ilk_fbc_wm_reg_max();
}

static void wm_ilk_compute_reg_maximums(struct lg_display *d, int level, struct wm_ilk_max *max)
{
	max->pri = wm_ilk_plane_wm_reg_max(d, level, 0);
	max->spr = wm_ilk_plane_wm_reg_max(d, level, 1);
	max->cur = wm_ilk_cursor_wm_reg_max(d, level);
	max->fbc = wm_ilk_fbc_wm_reg_max();
}

static int wm_ilk_validate_level(int level, const struct wm_ilk_max *max,
				 struct wm_ilk_level *result)
{
	int ret;

	/* already found unusable? */
	if (!result->enable)
		return 0;

	result->enable = result->pri_val <= max->pri && result->spr_val <= max->spr &&
			 result->cur_val <= max->cur;

	ret = result->enable;

	/* Level 0 cannot be turned off: clamp it to the maximums (and
	 * report the level as invalid). */
	if (level == 0 && !result->enable) {
		if (result->pri_val > max->pri)
			i915_dbg("[drm] i915: wm: primary WM%d too large %u (max %u)\n", level,
				 result->pri_val, max->pri);
		if (result->spr_val > max->spr)
			i915_dbg("[drm] i915: wm: sprite WM%d too large %u (max %u)\n", level,
				 result->spr_val, max->spr);
		if (result->cur_val > max->cur)
			i915_dbg("[drm] i915: wm: cursor WM%d too large %u (max %u)\n", level,
				 result->cur_val, max->cur);

		result->pri_val = wm_min_u32(result->pri_val, max->pri);
		result->spr_val = wm_min_u32(result->spr_val, max->spr);
		result->cur_val = wm_min_u32(result->cur_val, max->cur);
		result->enable = 1;
	}

	return ret;
}

static void wm_ilk_compute_level(const struct wm_in *in, int level,
				 struct wm_ilk_level *result)
{
	uint32_t pri_latency = g_wm.pri_latency[level];
	uint32_t spr_latency = g_wm.spr_latency[level];
	uint32_t cur_latency = g_wm.cur_latency[level];

	/* WM1+ latencies are in 0.5 us units */
	if (level > 0) {
		pri_latency *= 5;
		spr_latency *= 5;
		cur_latency *= 5;
	}

	result->pri_val = wm_ilk_compute_pri_wm(in, pri_latency, level > 0);
	result->fbc_val = wm_compute_fbc_wm(in, result->pri_val);
	result->spr_val = wm_ilk_compute_spr_wm(in, spr_latency);
	result->cur_val = wm_ilk_compute_cur_wm(in, cur_latency);
	result->enable = 1;
}

static void wm_ilk_print_latency(const char *name, const uint16_t wm[ILK_MAX_LEVELS])
{
	int level;

	for (level = 0; level < g_wm.num_levels; level++) {
		uint32_t latency = wm[level];

		if (latency == 0) {
			i915_dbg("[drm] i915: wm: %s WM%d latency not provided\n", name, level);
			continue;
		}

		/* WM1+ latencies are in 0.5 us units */
		if (level > 0)
			latency *= 5;

		i915_dbg("[drm] i915: wm: %s WM%d latency %u (%u.%u usec)\n", name, level,
			 wm[level], latency / 10, latency % 10);
	}
}

static int wm_ilk_increase_latency(uint16_t wm[ILK_MAX_LEVELS], uint16_t min)
{
	int level;

	if (wm[0] >= min)
		return 0;

	wm[0] = min;
	for (level = 1; level < g_wm.num_levels; level++)
		if (wm[level] < DIV_ROUND_UP(min, 5))
			wm[level] = (uint16_t)DIV_ROUND_UP(min, 5);

	return 1;
}

static void wm_ilk_setup_latency(struct lg_display *d)
{
	uint16_t *wm = g_wm.pri_latency;
	uint32_t val;

	mm_memset(g_wm.pri_latency, 0, sizeof(g_wm.pri_latency));

	if (d->ver >= 6) {
		/* the memory controller's latencies: level 0 in 0.1 us,
		 * the LP levels in 0.5 us */
		g_wm.num_levels = 4;
		val = lg_rd_abs(d, MCH_SSKPD_SNB);
		wm[0] = (uint16_t)SSKPD_WM0_SNB(val);
		wm[1] = (uint16_t)SSKPD_WM1_SNB(val);
		wm[2] = (uint16_t)SSKPD_WM2_SNB(val);
		wm[3] = (uint16_t)SSKPD_WM3_SNB(val);
	} else {
		g_wm.num_levels = 3;
		val = lg_rd_abs(d, MLTR_ILK);
		/* the ILK primary plane's LP0 latency is 700 ns */
		wm[0] = 7;
		wm[1] = (uint16_t)MLTR_WM1(val);
		wm[2] = (uint16_t)MLTR_WM2(val);
	}

	mm_memcpy(g_wm.spr_latency, g_wm.pri_latency, sizeof(g_wm.pri_latency));
	mm_memcpy(g_wm.cur_latency, g_wm.pri_latency, sizeof(g_wm.pri_latency));

	/* the ILK sprite and cursor LP0 latency is 1300 ns */
	if (d->ver == 5) {
		g_wm.spr_latency[0] = 13;
		g_wm.cur_latency[0] = 13;
	}

	wm_ilk_print_latency("Primary", g_wm.pri_latency);
	wm_ilk_print_latency("Sprite", g_wm.spr_latency);
	wm_ilk_print_latency("Cursor", g_wm.cur_latency);

	if (d->ver == 6) {
		int changed;

		/* The latencies the BIOS provides are often too low for
		 * high resolution displays: raise them. */
		changed = wm_ilk_increase_latency(g_wm.pri_latency, 12);
		changed |= wm_ilk_increase_latency(g_wm.spr_latency, 12);
		changed |= wm_ilk_increase_latency(g_wm.cur_latency, 12);
		if (changed) {
			i915_dbg("[drm] i915: wm: WM latency values increased to avoid potential underruns\n");
			wm_ilk_print_latency("Primary", g_wm.pri_latency);
			wm_ilk_print_latency("Sprite", g_wm.spr_latency);
			wm_ilk_print_latency("Cursor", g_wm.cur_latency);
		}

		/* On some Sandy Bridge machines LP3 makes vblank interrupts
		 * go missing (the DEIIR bit rises, the CPU never hears of
		 * it); whether other interrupts suffer too is unknown, so
		 * LP3 is never used. */
		if (g_wm.pri_latency[3] || g_wm.spr_latency[3] || g_wm.cur_latency[3]) {
			g_wm.pri_latency[3] = 0;
			g_wm.spr_latency[3] = 0;
			g_wm.cur_latency[3] = 0;
			i915_dbg("[drm] i915: wm: LP3 watermarks disabled due to potential for lost interrupts\n");
		}
	}
}

/* Compute a pipe's watermarks for every level. */
static void wm_ilk_compute_pipe(struct lg_display *d, int pipe, struct wm_ilk_pipe *pipe_wm)
{
	/* LP0 maxima depend on this pipe alone */
	const struct wm_ilk_config lp0_config = { .num_pipes_active = 1 };
	struct wm_ilk_max max;
	struct wm_in in;
	int level, usable_level;

	wm_get_in(d, pipe, &in);
	mm_memset(pipe_wm, 0, sizeof(*pipe_wm));

	pipe_wm->pipe_enabled = in.active;
	pipe_wm->sprites_enabled = 0;
	pipe_wm->sprites_scaled = 0;

	usable_level = g_wm.num_levels - 1;

	/* ILK/SNB: LP2+ only without sprites */
	if (d->ver < 7 && pipe_wm->sprites_enabled)
		usable_level = 1;

	/* ILK/SNB/IVB: LP1+ only without sprite scaling */
	if (pipe_wm->sprites_scaled)
		usable_level = 0;

	wm_ilk_compute_level(&in, 0, &pipe_wm->wm[0]);

	/* LP0 is always computed with the 1/2 split */
	wm_ilk_compute_maximums(d, 0, &lp0_config, ILK_DDB_PART_1_2, &max);
	if (!wm_ilk_validate_level(0, &max, &pipe_wm->wm[0]))
		kprintf("[drm] i915: watermarks: pipe %c LP0 watermark exceeds the FIFO\n",
			wm_pipe_name(pipe));

	wm_ilk_compute_reg_maximums(d, 1, &max);

	for (level = 1; level <= usable_level; level++) {
		struct wm_ilk_level *wm = &pipe_wm->wm[level];

		wm_ilk_compute_level(&in, level, wm);

		/* a level beyond the registers' maximums is never usable */
		if (!wm_ilk_validate_level(level, &max, wm)) {
			mm_memset(wm, 0, sizeof(*wm));
			break;
		}
	}
}

/* One LP level merged over the active pipes. */
static void wm_ilk_merge_level(struct lg_display *d, const struct wm_ilk_pipe pipes[],
			       int level, struct wm_ilk_level *ret_wm)
{
	int pipe;

	ret_wm->enable = 1;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		const struct wm_ilk_level *wm = &pipes[pipe].wm[level];

		if (!pipes[pipe].pipe_enabled)
			continue;

		/* The values may have been in use: keep them in the
		 * registers even when the level goes off. */
		if (!wm->enable)
			ret_wm->enable = 0;

		ret_wm->pri_val = wm_max_u32(ret_wm->pri_val, wm->pri_val);
		ret_wm->spr_val = wm_max_u32(ret_wm->spr_val, wm->spr_val);
		ret_wm->cur_val = wm_max_u32(ret_wm->cur_val, wm->cur_val);
		ret_wm->fbc_val = wm_max_u32(ret_wm->fbc_val, wm->fbc_val);
	}
}

static void wm_ilk_merge(struct lg_display *d, const struct wm_ilk_pipe pipes[],
			 const struct wm_ilk_config *config, const struct wm_ilk_max *max,
			 struct wm_ilk_pipe *merged)
{
	int level, num_levels = g_wm.num_levels;
	int last_enabled_level = num_levels - 1;

	mm_memset(merged, 0, sizeof(*merged));

	/* ILK/SNB/IVB: LP1+ only with a single pipe */
	if (config->num_pipes_active > 1)
		last_enabled_level = 0;

	/* ILK: the FBC watermark must always be off */
	merged->fbc_wm_enabled = d->ver >= 6;

	for (level = 1; level < num_levels; level++) {
		struct wm_ilk_level *wm = &merged->wm[level];

		wm_ilk_merge_level(d, pipes, level, wm);

		if (level > last_enabled_level)
			wm->enable = 0;
		else if (!wm_ilk_validate_level(level, max, wm))
			/* and every level after it */
			last_enabled_level = level - 1;

		/* Rather turn the FBC watermarks off than a level. */
		if (wm->fbc_val > max->fbc) {
			if (wm->enable)
				merged->fbc_wm_enabled = 0;
			wm->fbc_val = 0;
		}
	}

	/* (Ironlake would also lose LP2+ with FBC running and its
	 * watermark off; the framebuffer compressor is never used.) */
}

static int wm_ilk_lp_to_level(int wm_lp, const struct wm_ilk_pipe *pipe_wm)
{
	/* LP1, LP2, LP3 are levels 1, 2, 3 or 1, 3, 4 */
	return wm_lp + (wm_lp >= 2 && pipe_wm->wm[4].enable);
}

static void wm_ilk_compute_results(struct lg_display *d, const struct wm_ilk_pipe pipes[],
				   const struct wm_ilk_pipe *merged, int partitioning,
				   struct wm_ilk_values *results)
{
	int pipe, level, wm_lp;

	results->enable_fbc_wm = merged->fbc_wm_enabled;
	results->partitioning = partitioning;

	/* LP1+ */
	for (wm_lp = 1; wm_lp <= 3; wm_lp++) {
		const struct wm_ilk_level *r;

		level = wm_ilk_lp_to_level(wm_lp, merged);
		r = &merged->wm[level];

		/* Keep the values even when the level is off: dropping
		 * them could underrun. */
		results->wm_lp[wm_lp - 1] = WM_LP_LATENCY(g_wm.pri_latency[level]) |
					    WM_LP_PRIMARY(r->pri_val) | WM_LP_CURSOR(r->cur_val);

		if (r->enable)
			results->wm_lp[wm_lp - 1] |= WM_LP_ENABLE;

		results->wm_lp[wm_lp - 1] |= WM_LP_FBC_ILK(r->fbc_val);

		results->wm_lp_spr[wm_lp - 1] = WM_LP_SPRITE(r->spr_val);

		/* The sprite enable goes with any sprite value, even on a
		 * level that is off, or it could underrun. */
		if (d->ver < 7 && r->spr_val)
			results->wm_lp_spr[wm_lp - 1] |= WM_LP_SPRITE_ENABLE;
	}

	/* LP0 */
	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		const struct wm_ilk_level *r = &pipes[pipe].wm[0];

		if (!r->enable)
			continue;

		results->wm_pipe[pipe] = WM0_PIPE_PRIMARY(r->pri_val) |
					 WM0_PIPE_SPRITE(r->spr_val) | WM0_PIPE_CURSOR(r->cur_val);
	}
}

/* The result with the highest enabled level; with a tie, the one with
 * the FBC watermark on, else r1. */
static const struct wm_ilk_pipe *wm_ilk_find_best_result(const struct wm_ilk_pipe *r1,
							 const struct wm_ilk_pipe *r2)
{
	int level, level1 = 0, level2 = 0;

	for (level = 1; level < g_wm.num_levels; level++) {
		if (r1->wm[level].enable)
			level1 = level;
		if (r2->wm[level].enable)
			level2 = level;
	}

	if (level1 == level2) {
		if (r2->fbc_wm_enabled && !r1->fbc_wm_enabled)
			return r2;
		else
			return r1;
	} else if (level1 > level2) {
		return r1;
	} else {
		return r2;
	}
}

/* which watermarks need writing */
#define WM_DIRTY_PIPE(pipe) (1u << (pipe))
#define WM_DIRTY_LP(wm_lp) (1u << (15 + (wm_lp)))
#define WM_DIRTY_LP_ALL (WM_DIRTY_LP(1) | WM_DIRTY_LP(2) | WM_DIRTY_LP(3))
#define WM_DIRTY_FBC (1u << 24)
#define WM_DIRTY_DDB (1u << 25)

static uint32_t wm_ilk_compute_dirty(struct lg_display *d, const struct wm_ilk_values *old,
				     const struct wm_ilk_values *new)
{
	uint32_t dirty = 0;
	int pipe, wm_lp;

	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		if (old->wm_pipe[pipe] != new->wm_pipe[pipe]) {
			dirty |= WM_DIRTY_PIPE(pipe);
			/* the LP1+ watermarks must go off too */
			dirty |= WM_DIRTY_LP_ALL;
		}
	}

	if (old->enable_fbc_wm != new->enable_fbc_wm) {
		dirty |= WM_DIRTY_FBC;
		dirty |= WM_DIRTY_LP_ALL;
	}

	if (old->partitioning != new->partitioning) {
		dirty |= WM_DIRTY_DDB;
		dirty |= WM_DIRTY_LP_ALL;
	}

	if (dirty & WM_DIRTY_LP_ALL)
		return dirty;

	/* the lowest LP level that changes, and all above it */
	for (wm_lp = 1; wm_lp <= 3; wm_lp++) {
		if (old->wm_lp[wm_lp - 1] != new->wm_lp[wm_lp - 1] ||
		    old->wm_lp_spr[wm_lp - 1] != new->wm_lp_spr[wm_lp - 1])
			break;
	}

	for (; wm_lp <= 3; wm_lp++)
		dirty |= WM_DIRTY_LP(wm_lp);

	return dirty;
}

/* Turn the dirty LP levels off, highest first.  The sprite enable is
 * left alone: touching it could underrun. */
static int wm_ilk_disable_lp_wm(struct lg_display *d, uint32_t dirty)
{
	struct wm_ilk_values *previous = &g_wm.ilk_hw;
	int changed = 0;

	if ((dirty & WM_DIRTY_LP(3)) && (previous->wm_lp[2] & WM_LP_ENABLE)) {
		previous->wm_lp[2] &= ~WM_LP_ENABLE;
		lg_wr(d, WM3_LP_ILK, previous->wm_lp[2]);
		changed = 1;
	}
	if ((dirty & WM_DIRTY_LP(2)) && (previous->wm_lp[1] & WM_LP_ENABLE)) {
		previous->wm_lp[1] &= ~WM_LP_ENABLE;
		lg_wr(d, WM2_LP_ILK, previous->wm_lp[1]);
		changed = 1;
	}
	if ((dirty & WM_DIRTY_LP(1)) && (previous->wm_lp[0] & WM_LP_ENABLE)) {
		previous->wm_lp[0] &= ~WM_LP_ENABLE;
		lg_wr(d, WM1_LP_ILK, previous->wm_lp[0]);
		changed = 1;
	}

	return changed;
}

/* Every write makes the hardware re-evaluate the watermarks, which costs
 * power: write only what changed. */
static void wm_ilk_write_values(struct lg_display *d, const struct wm_ilk_values *results)
{
	struct wm_ilk_values *previous = &g_wm.ilk_hw;
	uint32_t dirty;

	dirty = wm_ilk_compute_dirty(d, previous, results);
	if (!dirty)
		return;

	wm_ilk_disable_lp_wm(d, dirty);

	if (dirty & WM_DIRTY_PIPE(0))
		lg_wr(d, WM0_PIPE_ILK(0), results->wm_pipe[0]);
	if (dirty & WM_DIRTY_PIPE(1))
		lg_wr(d, WM0_PIPE_ILK(1), results->wm_pipe[1]);
	if (dirty & WM_DIRTY_PIPE(2))
		lg_wr(d, WM0_PIPE_ILK(2), results->wm_pipe[2]);

	if (dirty & WM_DIRTY_DDB)
		lg_rmw(d, DISP_ARB_CTL2, DISP_DATA_PARTITION_5_6,
		       results->partitioning == ILK_DDB_PART_1_2 ? 0 : DISP_DATA_PARTITION_5_6);

	if (dirty & WM_DIRTY_FBC)
		lg_rmw(d, DISP_ARB_CTL, DISP_FBC_WM_DIS,
		       results->enable_fbc_wm ? 0 : DISP_FBC_WM_DIS);

	if ((dirty & WM_DIRTY_LP(1)) && previous->wm_lp_spr[0] != results->wm_lp_spr[0])
		lg_wr(d, WM1S_LP_ILK, results->wm_lp_spr[0]);

	if (d->ver >= 7) {
		if ((dirty & WM_DIRTY_LP(2)) && previous->wm_lp_spr[1] != results->wm_lp_spr[1])
			lg_wr(d, WM2S_LP_IVB, results->wm_lp_spr[1]);
		if ((dirty & WM_DIRTY_LP(3)) && previous->wm_lp_spr[2] != results->wm_lp_spr[2])
			lg_wr(d, WM3S_LP_IVB, results->wm_lp_spr[2]);
	}

	if ((dirty & WM_DIRTY_LP(1)) && previous->wm_lp[0] != results->wm_lp[0])
		lg_wr(d, WM1_LP_ILK, results->wm_lp[0]);
	if ((dirty & WM_DIRTY_LP(2)) && previous->wm_lp[1] != results->wm_lp[1])
		lg_wr(d, WM2_LP_ILK, results->wm_lp[1]);
	if ((dirty & WM_DIRTY_LP(3)) && previous->wm_lp[2] != results->wm_lp[2])
		lg_wr(d, WM3_LP_ILK, results->wm_lp[2]);

	*previous = *results;
}

static void wm_ilk_update(struct lg_display *d)
{
	struct wm_ilk_pipe pipes[LG_MAX_PIPES];
	struct wm_ilk_pipe lp_wm_1_2, lp_wm_5_6;
	const struct wm_ilk_pipe *best_lp_wm;
	struct wm_ilk_config config;
	struct wm_ilk_values results;
	struct wm_ilk_max max;
	int partitioning;
	int pipe;

	mm_memset(&config, 0, sizeof(config));
	for (pipe = 0; pipe < d->num_pipes; pipe++) {
		wm_ilk_compute_pipe(d, pipe, &pipes[pipe]);

		if (!pipes[pipe].pipe_enabled)
			continue;
		config.sprites_enabled |= pipes[pipe].sprites_enabled;
		config.sprites_scaled |= pipes[pipe].sprites_scaled;
		config.num_pipes_active++;
	}

	wm_ilk_compute_maximums(d, 1, &config, ILK_DDB_PART_1_2, &max);
	wm_ilk_merge(d, pipes, &config, &max, &lp_wm_1_2);

	/* the 5/6 split only for a single pipe with sprites on IVB */
	if (d->ver >= 7 && config.num_pipes_active == 1 && config.sprites_enabled) {
		wm_ilk_compute_maximums(d, 1, &config, ILK_DDB_PART_5_6, &max);
		wm_ilk_merge(d, pipes, &config, &max, &lp_wm_5_6);
		best_lp_wm = wm_ilk_find_best_result(&lp_wm_1_2, &lp_wm_5_6);
	} else {
		best_lp_wm = &lp_wm_1_2;
	}

	partitioning = best_lp_wm == &lp_wm_1_2 ? ILK_DDB_PART_1_2 : ILK_DDB_PART_5_6;

	mm_memset(&results, 0, sizeof(results));
	wm_ilk_compute_results(d, pipes, best_lp_wm, partitioning, &results);

	i915_dbg("[drm] i915: wm: ILK WM0 %08x %08x %08x LP %08x %08x %08x\n",
		 results.wm_pipe[0], results.wm_pipe[1], results.wm_pipe[2], results.wm_lp[0],
		 results.wm_lp[1], results.wm_lp[2]);

	wm_ilk_write_values(d, &results);
}

/* Turn the LP watermarks off before anything else: the firmware's are
 * for a configuration that is about to change. */
static void wm_ilk_init_lp_watermarks(struct lg_display *d)
{
	lg_rmw(d, WM3_LP_ILK, WM_LP_ENABLE, 0);
	lg_rmw(d, WM2_LP_ILK, WM_LP_ENABLE, 0);
	lg_rmw(d, WM1_LP_ILK, WM_LP_ENABLE, 0);

	/* the sprite enable stays: touching it could underrun */
}

static void wm_ilk_get_hw_state(struct lg_display *d)
{
	struct wm_ilk_values *hw = &g_wm.ilk_hw;
	int pipe;

	mm_memset(hw, 0, sizeof(*hw));

	wm_ilk_init_lp_watermarks(d);

	for (pipe = 0; pipe < d->num_pipes; pipe++)
		hw->wm_pipe[pipe] = lg_rd(d, WM0_PIPE_ILK(pipe));

	hw->wm_lp[0] = lg_rd(d, WM1_LP_ILK);
	hw->wm_lp[1] = lg_rd(d, WM2_LP_ILK);
	hw->wm_lp[2] = lg_rd(d, WM3_LP_ILK);

	hw->wm_lp_spr[0] = lg_rd(d, WM1S_LP_ILK);
	if (d->ver >= 7) {
		hw->wm_lp_spr[1] = lg_rd(d, WM2S_LP_IVB);
		hw->wm_lp_spr[2] = lg_rd(d, WM3S_LP_IVB);
	}

	if (d->is_ivb)
		hw->partitioning = (lg_rd(d, DISP_ARB_CTL2) & DISP_DATA_PARTITION_5_6) ?
					   ILK_DDB_PART_5_6 :
					   ILK_DDB_PART_1_2;

	hw->enable_fbc_wm = !(lg_rd(d, DISP_ARB_CTL) & DISP_FBC_WM_DIS);
}

/* ---- entry points ----------------------------------------------------------------- */

void lg_wm_init(struct lg_display *d)
{
	mm_memset(&g_wm, 0, sizeof(g_wm));

	if (d->is_ilk || d->is_snb || d->is_ivb) {
		g_wm.family = WM_FAM_ILK;
		wm_ilk_setup_latency(d);
		/* the LP watermarks off; level 0 stays as it is until the
		 * first update */
		wm_ilk_get_hw_state(d);
	} else if (d->is_vlv || d->is_chv) {
		g_wm.family = WM_FAM_VLV;
		wm_vlv_setup_latency(d);
		wm_vlv_get_hw_state(d);
		wm_set_cxsr(d, 0);
	} else if (d->is_g4x || d->is_gm45) {
		g_wm.family = WM_FAM_G4X;
		wm_g4x_setup_latency();
		wm_g4x_read_values(d, &g_wm.g4x);
		g_wm.g4x.cxsr = !!(lg_rd(d, FW_BLC_SELF) & FW_BLC_SELF_EN);
		g_wm.g4x_valid = 1;
		i915_dbg("[drm] i915: wm: initial SR plane=%u cursor=%u fbc=%u, HPLL plane=%u cursor=%u fbc=%u, SR=%u HPLL=%u FBC=%u\n",
			 g_wm.g4x.sr.plane, g_wm.g4x.sr.cursor, g_wm.g4x.sr.fbc,
			 g_wm.g4x.hpll.plane, g_wm.g4x.hpll.cursor, g_wm.g4x.hpll.fbc,
			 g_wm.g4x.cxsr, g_wm.g4x.hpll_en, g_wm.g4x.fbc_en);
		wm_set_cxsr(d, 0);
	} else if (d->is_pnv) {
		g_wm.family = WM_FAM_PNV;
		g_wm.pnv_latency = wm_pnv_get_cxsr_latency(d);
		if (!g_wm.pnv_latency)
			/* self refresh off, and never touched again */
			kprintf("[drm] i915: unknown FSB/MEM (%u/%u kHz), disabling CxSR\n",
				d->fsb_khz, d->mem_khz);
		wm_set_cxsr(d, 0);
	} else if (d->ver == 4) {
		g_wm.family = WM_FAM_I965;
		wm_set_cxsr(d, 0);
	} else if (d->ver == 3) {
		g_wm.family = WM_FAM_I9XX;
		wm_i9xx_get_fifo_size(d, 0);
		wm_i9xx_get_fifo_size(d, 1);
		wm_set_cxsr(d, 0);
	} else if (d->ver == 2) {
		if (d->num_pipes == 1) {
			g_wm.family = WM_FAM_I845;
			wm_i845_get_fifo_size(d, 0);
		} else {
			g_wm.family = WM_FAM_I9XX;
			wm_i830_get_fifo_size(d, 0);
			wm_i830_get_fifo_size(d, 1);
		}
	} else {
		kprintf("[drm] i915: watermarks: unexpected display version %d\n", d->ver);
		g_wm.family = WM_FAM_NONE;
	}
}

void lg_wm_update(struct lg_display *d)
{
	switch (g_wm.family) {
	case WM_FAM_ILK:
		wm_ilk_update(d);
		break;
	case WM_FAM_VLV:
		wm_vlv_update(d);
		break;
	case WM_FAM_G4X:
		wm_g4x_update(d);
		break;
	case WM_FAM_PNV:
		/* without a latency for the memory, nothing to do */
		if (g_wm.pnv_latency)
			wm_pnv_update(d);
		break;
	case WM_FAM_I965:
		wm_i965_update(d);
		break;
	case WM_FAM_I9XX:
		wm_i9xx_update(d);
		break;
	case WM_FAM_I845:
		wm_i845_update(d);
		break;
	default:
		break;
	}
}

void lg_wm_cxsr_disable(struct lg_display *d)
{
	switch (g_wm.family) {
	case WM_FAM_ILK:
		/* the LP levels are the PCH parts' self refresh */
		wm_ilk_disable_lp_wm(d, WM_DIRTY_LP_ALL);
		break;
	case WM_FAM_NONE:
		break;
	default:
		wm_set_cxsr(d, 0);
		break;
	}
}
