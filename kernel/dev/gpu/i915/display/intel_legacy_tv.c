// LikeOS -- the integrated TV encoder of the mobile gen3 and gen4 parts.
//
// The i915GM, i945GM, i965GM and GM45 carry a TV encoder of their own: it
// takes the picture of pipe A or B, scales it with a polyphase filter into
// the active area of a broadcast format (NTSC, PAL and their variants at
// 108 MHz with 8x oversampling, or the component formats 480p to 1080p),
// adds the sync, equalisation and colour burst the format wants, converts
// the RGB stream to YUV or YPbPr with a programmable matrix, and drives it
// out of three DACs as composite, S-video or component video.  The pipe
// runs from the TV clock and is stalled by the encoder line by line, so
// its timing is that of the TV format scaled to the size the plane shows.
// There is no hotplug: a TV is found by forcing the DACs to half level on
// a running pipe and sensing which of the three lines carry a load.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2023 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* ---- registers ------------------------------------------------------------------ */

/* TV port control */
#define TV_CTL 0x68000u
#define TV_ENC_ENABLE (1u << 31)
/* the encoder's input: pipe A or B */
#define TV_ENC_PIPE_SEL_SHIFT 30
#define TV_ENC_PIPE_SEL_MASK (1u << 30)
#define TV_ENC_PIPE_SEL(pipe) ((uint32_t)(pipe) << 30)
/* composite on DAC A, S-video on B/C, component on all three */
#define TV_ENC_OUTPUT_COMPOSITE (0u << 28)
#define TV_ENC_OUTPUT_SVIDEO (1u << 28)
#define TV_ENC_OUTPUT_COMPONENT (2u << 28)
#define TV_ENC_OUTPUT_SVIDEO_COMPOSITE (3u << 28)
#define TV_TRILEVEL_SYNC (1u << 21)
#define TV_SLOW_SYNC (1u << 20) /* 945GM */
#define TV_OVERSAMPLE_4X (0u << 18)
#define TV_OVERSAMPLE_2X (1u << 18)
#define TV_OVERSAMPLE_NONE (2u << 18)
#define TV_OVERSAMPLE_8X (3u << 18)
#define TV_OVERSAMPLE_MASK (3u << 18)
#define TV_PROGRESSIVE (1u << 17)
/* PAL colour burst, needed by every PAL format but PAL-M's */
#define TV_PAL_BURST (1u << 16)
#define TV_YC_SKEW_MASK (7u << 12)
/* two fixes of the 915GM (480p/576p and an unexplained one) */
#define TV_ENC_SDP_FIX (1u << 11)
#define TV_ENC_C0_FIX (1u << 10)
/* the bits software must preserve */
#define TV_CTL_SAVE ((1u << 11) | (3u << 9) | (7u << 6) | 0xfu)
#define TV_FUSE_STATE_MASK (3u << 4)
#define TV_FUSE_STATE_ENABLED (0u << 4)
#define TV_FUSE_STATE_NO_MACROVISION (1u << 4)
#define TV_FUSE_STATE_DISABLED (2u << 4)
#define TV_TEST_MODE_NORMAL (0u << 0)
/* the DACs forced to half of full output, for load detection */
#define TV_TEST_MODE_MONITOR_DETECT (7u << 0)
#define TV_TEST_MODE_MASK (7u << 0)

#define TV_DAC 0x68004u
#define TV_DAC_SAVE 0x00ffff00u
#define TVDAC_STATE_CHG (1u << 31)
#define TVDAC_SENSE_MASK (7u << 28)
#define TVDAC_A_SENSE (1u << 30)
#define TVDAC_B_SENSE (1u << 29)
#define TVDAC_C_SENSE (1u << 28)
/* The state detection logic: the pipe named in TV_CTL must be running
 * and the encoder off for it to sense a load. */
#define TVDAC_STATE_CHG_EN (1u << 27)
#define TVDAC_A_SENSE_CTL (1u << 26)
#define TVDAC_B_SENSE_CTL (1u << 25)
#define TVDAC_C_SENSE_CTL (1u << 24)
/* overrides the encoder enable and the DAC levels */
#define TV_DAC_CTL_OVERRIDE (1u << 7)
#define TV_DAC_SLEW_FAST (1u << 6)
#define TV_DAC_A_0_7_V (2u << 4)
#define TV_DAC_A_MASK (3u << 4)
#define TV_DAC_B_0_7_V (2u << 2)
#define TV_DAC_B_MASK (3u << 2)
#define TV_DAC_C_0_7_V (2u << 0)
#define TV_DAC_C_MASK (3u << 0)

/* The colour conversion matrix: coefficients with a 9-bit mantissa and
 * a 2- or 3-bit exponent, attenuations in 1.9 fixed point. */
#define TV_CSC_Y 0x68010u
#define TV_CSC_Y2 0x68014u
#define TV_CSC_U 0x68018u
#define TV_CSC_U2 0x6801cu
#define TV_CSC_V 0x68020u
#define TV_CSC_V2 0x68024u

/* brightness, contrast, saturation, hue */
#define TV_CLR_KNOBS 0x68028u
#define TV_CLR_LEVEL 0x6802cu
#define TV_BLACK_LEVEL_SHIFT 16
#define TV_BLANK_LEVEL_SHIFT 0

#define TV_H_CTL_1 0x68030u
#define TV_HSYNC_END_MASK 0x1fff0000u
#define TV_HSYNC_END_SHIFT 16
#define TV_HTOTAL_MASK 0x00001fffu
#define TV_HTOTAL_SHIFT 0
#define TV_H_CTL_2 0x68034u
#define TV_BURST_ENA (1u << 31)
#define TV_HBURST_START_SHIFT 16
#define TV_HBURST_LEN_SHIFT 0
#define TV_H_CTL_3 0x68038u
#define TV_HBLANK_END_SHIFT 16
#define TV_HBLANK_START_SHIFT 0
#define TV_V_CTL_1 0x6803cu
#define TV_NBR_END_SHIFT 16
#define TV_VI_END_F1_SHIFT 8
#define TV_VI_END_F2_SHIFT 0
#define TV_V_CTL_2 0x68040u
#define TV_VSYNC_LEN_SHIFT 16
#define TV_VSYNC_START_F1_SHIFT 8
#define TV_VSYNC_START_F2_SHIFT 0
#define TV_V_CTL_3 0x68044u
#define TV_EQUAL_ENA (1u << 31)
#define TV_VEQ_LEN_SHIFT 16
#define TV_VEQ_START_F1_SHIFT 8
#define TV_VEQ_START_F2_SHIFT 0
#define TV_V_CTL_4 0x68048u
#define TV_VBURST_START_F1_SHIFT 16
#define TV_VBURST_END_F1_SHIFT 0
#define TV_V_CTL_5 0x6804cu
#define TV_VBURST_START_F2_SHIFT 16
#define TV_VBURST_END_F2_SHIFT 0
#define TV_V_CTL_6 0x68050u
#define TV_VBURST_START_F3_SHIFT 16
#define TV_VBURST_END_F3_SHIFT 0
#define TV_V_CTL_7 0x68054u
#define TV_VBURST_START_F4_SHIFT 16
#define TV_VBURST_END_F4_SHIFT 0

/* the subcarrier DDAs */
#define TV_SC_CTL_1 0x68060u
#define TV_SC_DDA1_EN (1u << 31)
#define TV_SC_DDA2_EN (1u << 30)
#define TV_SC_DDA3_EN (1u << 29)
#define TV_SC_RESET_EVERY_2 (0u << 24)
#define TV_SC_RESET_EVERY_4 (1u << 24)
#define TV_SC_RESET_EVERY_8 (2u << 24)
#define TV_SC_RESET_NEVER (3u << 24)
#define TV_BURST_LEVEL_SHIFT 16
#define TV_SCDDA1_INC_SHIFT 0
#define TV_SC_CTL_2 0x68064u
#define TV_SCDDA2_SIZE_SHIFT 16
#define TV_SCDDA2_INC_SHIFT 0
#define TV_SC_CTL_3 0x68068u
#define TV_SCDDA3_SIZE_SHIFT 16
#define TV_SCDDA3_INC_SHIFT 0

/* where in the active area the picture goes, and how big */
#define TV_WIN_POS 0x68070u
#define TV_WIN_SIZE 0x68074u

#define TV_FILTER_CTL_1 0x68080u
/* the scaling factors computed by the hardware from TV_WIN_SIZE */
#define TV_AUTO_SCALE (1u << 31)
/* the vertical filter off (wide sources) */
#define TV_V_FILTER_BYPASS (1u << 29)
#define TV_FILTER_CTL_2 0x68084u
#define TV_FILTER_CTL_3 0x68088u

/* the scaler's filter coefficients */
#define TV_H_LUMA(i) (0x68100u + (uint32_t)(i) * 4u) /* 60 registers */
#define TV_H_CHROMA(i) (0x68200u + (uint32_t)(i) * 4u) /* 60 registers */
#define TV_V_LUMA(i) (0x68300u + (uint32_t)(i) * 4u) /* 43 registers */
#define TV_V_CHROMA(i) (0x68400u + (uint32_t)(i) * 4u) /* 43 registers */

#define TV_NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- the formats ------------------------------------------------------------------ */

struct tv_video_levels {
	uint16_t blank, black;
	uint8_t burst;
};

struct tv_color_conversion {
	uint16_t ry, gy, by, ay;
	uint16_t ru, gu, bu, au;
	uint16_t rv, gv, bv, av;
};

/* The polyphase scaler's coefficients: horizontal luma and chroma (60
 * each), then vertical luma and chroma (43 each). */
static const uint32_t tv_filter_table[] = {
	0xB1403000, 0x2E203500, 0x35002E20, 0x3000B140,
	0x35A0B160, 0x2DC02E80, 0xB1403480, 0xB1603000,
	0x2EA03640, 0x34002D80, 0x3000B120, 0x36E0B160,
	0x2D202EF0, 0xB1203380, 0xB1603000, 0x2F303780,
	0x33002CC0, 0x3000B100, 0x3820B160, 0x2C802F50,
	0xB10032A0, 0xB1603000, 0x2F9038C0, 0x32202C20,
	0x3000B0E0, 0x3980B160, 0x2BC02FC0, 0xB0E031C0,
	0xB1603000, 0x2FF03A20, 0x31602B60, 0xB020B0C0,
	0x3AE0B160, 0x2B001810, 0xB0C03120, 0xB140B020,
	0x18283BA0, 0x30C02A80, 0xB020B0A0, 0x3C60B140,
	0x2A201838, 0xB0A03080, 0xB120B020, 0x18383D20,
	0x304029C0, 0xB040B080, 0x3DE0B100, 0x29601848,
	0xB0803000, 0xB100B040, 0x18483EC0, 0xB0402900,
	0xB040B060, 0x3F80B0C0, 0x28801858, 0xB060B080,
	0xB0A0B060, 0x18602820, 0xB0A02820, 0x0000B060,
	0xB1403000, 0x2E203500, 0x35002E20, 0x3000B140,
	0x35A0B160, 0x2DC02E80, 0xB1403480, 0xB1603000,
	0x2EA03640, 0x34002D80, 0x3000B120, 0x36E0B160,
	0x2D202EF0, 0xB1203380, 0xB1603000, 0x2F303780,
	0x33002CC0, 0x3000B100, 0x3820B160, 0x2C802F50,
	0xB10032A0, 0xB1603000, 0x2F9038C0, 0x32202C20,
	0x3000B0E0, 0x3980B160, 0x2BC02FC0, 0xB0E031C0,
	0xB1603000, 0x2FF03A20, 0x31602B60, 0xB020B0C0,
	0x3AE0B160, 0x2B001810, 0xB0C03120, 0xB140B020,
	0x18283BA0, 0x30C02A80, 0xB020B0A0, 0x3C60B140,
	0x2A201838, 0xB0A03080, 0xB120B020, 0x18383D20,
	0x304029C0, 0xB040B080, 0x3DE0B100, 0x29601848,
	0xB0803000, 0xB100B040, 0x18483EC0, 0xB0402900,
	0xB040B060, 0x3F80B0C0, 0x28801858, 0xB060B080,
	0xB0A0B060, 0x18602820, 0xB0A02820, 0x0000B060,
	0x36403000, 0x2D002CC0, 0x30003640, 0x2D0036C0,
	0x35C02CC0, 0x37403000, 0x2C802D40, 0x30003540,
	0x2D8037C0, 0x34C02C40, 0x38403000, 0x2BC02E00,
	0x30003440, 0x2E2038C0, 0x34002B80, 0x39803000,
	0x2B402E40, 0x30003380, 0x2E603A00, 0x33402B00,
	0x3A803040, 0x2A802EA0, 0x30403300, 0x2EC03B40,
	0x32802A40, 0x3C003040, 0x2A002EC0, 0x30803240,
	0x2EC03C80, 0x320029C0, 0x3D403080, 0x29402F00,
	0x308031C0, 0x2F203DC0, 0x31802900, 0x3E8030C0,
	0x28802F40, 0x30C03140, 0x2F203F40, 0x31402840,
	0x28003100, 0x28002F00, 0x00003100, 0x36403000,
	0x2D002CC0, 0x30003640, 0x2D0036C0,
	0x35C02CC0, 0x37403000, 0x2C802D40, 0x30003540,
	0x2D8037C0, 0x34C02C40, 0x38403000, 0x2BC02E00,
	0x30003440, 0x2E2038C0, 0x34002B80, 0x39803000,
	0x2B402E40, 0x30003380, 0x2E603A00, 0x33402B00,
	0x3A803040, 0x2A802EA0, 0x30403300, 0x2EC03B40,
	0x32802A40, 0x3C003040, 0x2A002EC0, 0x30803240,
	0x2EC03C80, 0x320029C0, 0x3D403080, 0x29402F00,
	0x308031C0, 0x2F203DC0, 0x31802900, 0x3E8030C0,
	0x28802F40, 0x30C03140, 0x2F203F40, 0x31402840,
	0x28003100, 0x28002F00, 0x00003100,
};

/*
 * The colour conversion values come in three fixed-point layouts:
 *
 *   10-bit fields (ay, au): 1.9 fixed point.
 *   11-bit fields (ry, by, ru, gu, gv): a 2-bit exponent over a 9-bit
 *     mantissa, the exponent e scaling the mantissa 0.mmmmmmmmm by 2^-e.
 *   12-bit fields (gy, rv, bu): the same with a 3-bit exponent, where
 *     the exponent 7 stands for 2^0 with an 8-bit fraction (m.mmmmmmmm),
 *     usable only to represent 1.0.
 *
 * Saturation and contrast are 8 bits, a 2-bit exponent over a 6-bit
 * mantissa.  The values below are already in the register encoding.
 *
 * The composite and S-video values of PAL and NTSC follow, then the
 * component ones.
 */
static const struct tv_color_conversion ntsc_m_csc_composite = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0104,
	.ru = 0x0733, .gu = 0x052d, .bu = 0x05c7, .au = 0x0200,
	.rv = 0x0340, .gv = 0x030c, .bv = 0x06d0, .av = 0x0200,
};

static const struct tv_video_levels ntsc_m_levels_composite = {
	.blank = 225, .black = 267, .burst = 113,
};

static const struct tv_color_conversion ntsc_m_csc_svideo = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0133,
	.ru = 0x076a, .gu = 0x0564, .bu = 0x030d, .au = 0x0200,
	.rv = 0x037a, .gv = 0x033d, .bv = 0x06f6, .av = 0x0200,
};

static const struct tv_video_levels ntsc_m_levels_svideo = {
	.blank = 266, .black = 316, .burst = 133,
};

static const struct tv_color_conversion ntsc_j_csc_composite = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0119,
	.ru = 0x074c, .gu = 0x0546, .bu = 0x05ec, .au = 0x0200,
	.rv = 0x035a, .gv = 0x0322, .bv = 0x06e1, .av = 0x0200,
};

static const struct tv_video_levels ntsc_j_levels_composite = {
	.blank = 225, .black = 225, .burst = 113,
};

static const struct tv_color_conversion ntsc_j_csc_svideo = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x014c,
	.ru = 0x0788, .gu = 0x0581, .bu = 0x0322, .au = 0x0200,
	.rv = 0x0399, .gv = 0x0356, .bv = 0x070a, .av = 0x0200,
};

static const struct tv_video_levels ntsc_j_levels_svideo = {
	.blank = 266, .black = 266, .burst = 133,
};

static const struct tv_color_conversion pal_csc_composite = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0113,
	.ru = 0x0745, .gu = 0x053f, .bu = 0x05e1, .au = 0x0200,
	.rv = 0x0353, .gv = 0x031c, .bv = 0x06dc, .av = 0x0200,
};

static const struct tv_video_levels pal_levels_composite = {
	.blank = 237, .black = 237, .burst = 118,
};

static const struct tv_color_conversion pal_csc_svideo = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0145,
	.ru = 0x0780, .gu = 0x0579, .bu = 0x031c, .au = 0x0200,
	.rv = 0x0390, .gv = 0x034f, .bv = 0x0705, .av = 0x0200,
};

static const struct tv_video_levels pal_levels_svideo = {
	.blank = 280, .black = 280, .burst = 139,
};

static const struct tv_color_conversion pal_m_csc_composite = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0104,
	.ru = 0x0733, .gu = 0x052d, .bu = 0x05c7, .au = 0x0200,
	.rv = 0x0340, .gv = 0x030c, .bv = 0x06d0, .av = 0x0200,
};

static const struct tv_video_levels pal_m_levels_composite = {
	.blank = 225, .black = 267, .burst = 113,
};

static const struct tv_color_conversion pal_m_csc_svideo = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0133,
	.ru = 0x076a, .gu = 0x0564, .bu = 0x030d, .au = 0x0200,
	.rv = 0x037a, .gv = 0x033d, .bv = 0x06f6, .av = 0x0200,
};

static const struct tv_video_levels pal_m_levels_svideo = {
	.blank = 266, .black = 316, .burst = 133,
};

static const struct tv_color_conversion pal_n_csc_composite = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0104,
	.ru = 0x0733, .gu = 0x052d, .bu = 0x05c7, .au = 0x0200,
	.rv = 0x0340, .gv = 0x030c, .bv = 0x06d0, .av = 0x0200,
};

static const struct tv_video_levels pal_n_levels_composite = {
	.blank = 225, .black = 267, .burst = 118,
};

static const struct tv_color_conversion pal_n_csc_svideo = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0133,
	.ru = 0x076a, .gu = 0x0564, .bu = 0x030d, .au = 0x0200,
	.rv = 0x037a, .gv = 0x033d, .bv = 0x06f6, .av = 0x0200,
};

static const struct tv_video_levels pal_n_levels_svideo = {
	.blank = 266, .black = 316, .burst = 139,
};

/*
 * Component connections
 */
static const struct tv_color_conversion sdtv_csc_yprpb = {
	.ry = 0x0332, .gy = 0x012d, .by = 0x07d3, .ay = 0x0145,
	.ru = 0x0559, .gu = 0x0353, .bu = 0x0100, .au = 0x0200,
	.rv = 0x0100, .gv = 0x03ad, .bv = 0x074d, .av = 0x0200,
};

static const struct tv_color_conversion hdtv_csc_yprpb = {
	.ry = 0x05b3, .gy = 0x016e, .by = 0x0728, .ay = 0x0145,
	.ru = 0x07d5, .gu = 0x038b, .bu = 0x0100, .au = 0x0200,
	.rv = 0x0100, .gv = 0x03d1, .bv = 0x06bc, .av = 0x0200,
};

static const struct tv_video_levels component_levels = {
	.blank = 279, .black = 279, .burst = 0,
};

struct tv_mode {
	const char *name;

	uint32_t clock; /* kHz */
	uint16_t refresh; /* in millihertz */
	uint8_t oversample;
	uint8_t hsync_end;
	uint16_t hblank_start, hblank_end, htotal;
	unsigned int progressive : 1, trilevel_sync : 1, component_only : 1;
	uint8_t vsync_start_f1, vsync_start_f2, vsync_len;
	unsigned int veq_ena : 1;
	uint8_t veq_start_f1, veq_start_f2, veq_len;
	uint8_t vi_end_f1, vi_end_f2;
	uint16_t nbr_end;
	unsigned int burst_ena : 1;
	uint8_t hburst_start, hburst_len;
	uint8_t vburst_start_f1;
	uint16_t vburst_end_f1;
	uint8_t vburst_start_f2;
	uint16_t vburst_end_f2;
	uint8_t vburst_start_f3;
	uint16_t vburst_end_f3;
	uint8_t vburst_start_f4;
	uint16_t vburst_end_f4;
	/* subcarrier programming */
	uint16_t dda2_size, dda3_size;
	uint8_t dda1_inc;
	uint16_t dda2_inc, dda3_inc;
	uint32_t sc_reset;
	unsigned int pal_burst : 1;
	/* blank/black levels and colour conversion */
	const struct tv_video_levels *composite_levels, *svideo_levels;
	const struct tv_color_conversion *composite_color, *svideo_color;
	const uint32_t *filter_table;
};

/*
 * The subcarrier DDA: the subcarrier frequency is
 *
 *   pixel_clock * (dda1_inc + dda2_inc / dda2_size) / 4096
 *
 * with dda3 trimming dda2's increment further where dda2 alone cannot
 * get close enough.  dda1_inc is the integer part of
 * subcarrier / pixel * 4096, dda2 the ratio nearest its fraction.  The
 * constants below were all computed for a 107.520 MHz clock.
 *
 * The register values of the formats; they include the -1s the
 * registers want.
 */
static const struct tv_mode tv_modes[] = {
	{
		.name		= "NTSC-M",
		.clock		= 108000,
		.refresh	= 59940,
		.oversample	= 8,
		.component_only = 0,
		/* 525 Lines, 60 Fields, 15.734KHz line, Sub-Carrier 3.580MHz */

		.hsync_end	= 64,		    .hblank_end		= 124,
		.hblank_start	= 836,		    .htotal		= 857,

		.progressive	= 0,	    .trilevel_sync = 0,

		.vsync_start_f1	= 6,		    .vsync_start_f2	= 7,
		.vsync_len	= 6,

		.veq_ena	= 1,		    .veq_start_f1	= 0,
		.veq_start_f2	= 1,		    .veq_len		= 18,

		.vi_end_f1	= 20,		    .vi_end_f2		= 21,
		.nbr_end	= 240,

		.burst_ena	= 1,
		.hburst_start	= 72,		    .hburst_len		= 34,
		.vburst_start_f1 = 9,		    .vburst_end_f1	= 240,
		.vburst_start_f2 = 10,		    .vburst_end_f2	= 240,
		.vburst_start_f3 = 9,		    .vburst_end_f3	= 240,
		.vburst_start_f4 = 10,		    .vburst_end_f4	= 240,

		/* desired 3.5800000 actual 3.5800000 clock 107.52 */
		.dda1_inc	=    135,
		.dda2_inc	=  20800,	    .dda2_size		=  27456,
		.dda3_inc	=      0,	    .dda3_size		=      0,
		.sc_reset	= TV_SC_RESET_EVERY_4,
		.pal_burst	= 0,

		.composite_levels = &ntsc_m_levels_composite,
		.composite_color = &ntsc_m_csc_composite,
		.svideo_levels  = &ntsc_m_levels_svideo,
		.svideo_color = &ntsc_m_csc_svideo,

		.filter_table = tv_filter_table,
	},
	{
		.name		= "NTSC-443",
		.clock		= 108000,
		.refresh	= 59940,
		.oversample	= 8,
		.component_only = 0,
		/* 525 Lines, 60 Fields, 15.734KHz line, Sub-Carrier 4.43MHz */
		.hsync_end	= 64,		    .hblank_end		= 124,
		.hblank_start	= 836,		    .htotal		= 857,

		.progressive	= 0,	    .trilevel_sync = 0,

		.vsync_start_f1 = 6,		    .vsync_start_f2	= 7,
		.vsync_len	= 6,

		.veq_ena	= 1,		    .veq_start_f1	= 0,
		.veq_start_f2	= 1,		    .veq_len		= 18,

		.vi_end_f1	= 20,		    .vi_end_f2		= 21,
		.nbr_end	= 240,

		.burst_ena	= 1,
		.hburst_start	= 72,		    .hburst_len		= 34,
		.vburst_start_f1 = 9,		    .vburst_end_f1	= 240,
		.vburst_start_f2 = 10,		    .vburst_end_f2	= 240,
		.vburst_start_f3 = 9,		    .vburst_end_f3	= 240,
		.vburst_start_f4 = 10,		    .vburst_end_f4	= 240,

		/* desired 4.4336180 actual 4.4336180 clock 107.52 */
		.dda1_inc       =    168,
		.dda2_inc       =   4093,       .dda2_size      =  27456,
		.dda3_inc       =    310,       .dda3_size      =    525,
		.sc_reset   = TV_SC_RESET_NEVER,
		.pal_burst  = 0,

		.composite_levels = &ntsc_m_levels_composite,
		.composite_color = &ntsc_m_csc_composite,
		.svideo_levels  = &ntsc_m_levels_svideo,
		.svideo_color = &ntsc_m_csc_svideo,

		.filter_table = tv_filter_table,
	},
	{
		.name		= "NTSC-J",
		.clock		= 108000,
		.refresh	= 59940,
		.oversample	= 8,
		.component_only = 0,

		/* 525 Lines, 60 Fields, 15.734KHz line, Sub-Carrier 3.580MHz */
		.hsync_end	= 64,		    .hblank_end		= 124,
		.hblank_start = 836,	    .htotal		= 857,

		.progressive	= 0,    .trilevel_sync = 0,

		.vsync_start_f1	= 6,	    .vsync_start_f2	= 7,
		.vsync_len	= 6,

		.veq_ena      = 1,	    .veq_start_f1	= 0,
		.veq_start_f2 = 1,	    .veq_len		= 18,

		.vi_end_f1	= 20,		    .vi_end_f2		= 21,
		.nbr_end	= 240,

		.burst_ena	= 1,
		.hburst_start	= 72,		    .hburst_len		= 34,
		.vburst_start_f1 = 9,		    .vburst_end_f1	= 240,
		.vburst_start_f2 = 10,		    .vburst_end_f2	= 240,
		.vburst_start_f3 = 9,		    .vburst_end_f3	= 240,
		.vburst_start_f4 = 10,		    .vburst_end_f4	= 240,

		/* desired 3.5800000 actual 3.5800000 clock 107.52 */
		.dda1_inc	=    135,
		.dda2_inc	=  20800,	    .dda2_size		=  27456,
		.dda3_inc	=      0,	    .dda3_size		=      0,
		.sc_reset	= TV_SC_RESET_EVERY_4,
		.pal_burst	= 0,

		.composite_levels = &ntsc_j_levels_composite,
		.composite_color = &ntsc_j_csc_composite,
		.svideo_levels  = &ntsc_j_levels_svideo,
		.svideo_color = &ntsc_j_csc_svideo,

		.filter_table = tv_filter_table,
	},
	{
		.name		= "PAL-M",
		.clock		= 108000,
		.refresh	= 59940,
		.oversample	= 8,
		.component_only = 0,

		/* 525 Lines, 60 Fields, 15.734KHz line, Sub-Carrier 3.580MHz */
		.hsync_end	= 64,		  .hblank_end		= 124,
		.hblank_start = 836,	  .htotal		= 857,

		.progressive	= 0,	    .trilevel_sync = 0,

		.vsync_start_f1	= 6,		    .vsync_start_f2	= 7,
		.vsync_len	= 6,

		.veq_ena	= 1,		    .veq_start_f1	= 0,
		.veq_start_f2	= 1,		    .veq_len		= 18,

		.vi_end_f1	= 20,		    .vi_end_f2		= 21,
		.nbr_end	= 240,

		.burst_ena	= 1,
		.hburst_start	= 72,		    .hburst_len		= 34,
		.vburst_start_f1 = 9,		    .vburst_end_f1	= 240,
		.vburst_start_f2 = 10,		    .vburst_end_f2	= 240,
		.vburst_start_f3 = 9,		    .vburst_end_f3	= 240,
		.vburst_start_f4 = 10,		    .vburst_end_f4	= 240,

		/* desired 3.5800000 actual 3.5800000 clock 107.52 */
		.dda1_inc	=    135,
		.dda2_inc	=  16704,	    .dda2_size		=  27456,
		.dda3_inc	=      0,	    .dda3_size		=      0,
		.sc_reset	= TV_SC_RESET_EVERY_8,
		.pal_burst  = 1,

		.composite_levels = &pal_m_levels_composite,
		.composite_color = &pal_m_csc_composite,
		.svideo_levels  = &pal_m_levels_svideo,
		.svideo_color = &pal_m_csc_svideo,

		.filter_table = tv_filter_table,
	},
	{
		/* 625 Lines, 50 Fields, 15.625KHz line, Sub-Carrier 4.434MHz */
		.name	    = "PAL-N",
		.clock		= 108000,
		.refresh	= 50000,
		.oversample	= 8,
		.component_only = 0,

		.hsync_end	= 64,		    .hblank_end		= 128,
		.hblank_start = 844,	    .htotal		= 863,

		.progressive  = 0,    .trilevel_sync = 0,


		.vsync_start_f1	= 6,	   .vsync_start_f2	= 7,
		.vsync_len	= 6,

		.veq_ena	= 1,		    .veq_start_f1	= 0,
		.veq_start_f2	= 1,		    .veq_len		= 18,

		.vi_end_f1	= 24,		    .vi_end_f2		= 25,
		.nbr_end	= 286,

		.burst_ena	= 1,
		.hburst_start = 73,	    .hburst_len		= 34,
		.vburst_start_f1 = 8,	    .vburst_end_f1	= 285,
		.vburst_start_f2 = 8,	    .vburst_end_f2	= 286,
		.vburst_start_f3 = 9,	    .vburst_end_f3	= 286,
		.vburst_start_f4 = 9,	    .vburst_end_f4	= 285,


		/* desired 4.4336180 actual 4.4336180 clock 107.52 */
		.dda1_inc       =    135,
		.dda2_inc       =  23578,       .dda2_size      =  27648,
		.dda3_inc       =    134,       .dda3_size      =    625,
		.sc_reset   = TV_SC_RESET_EVERY_8,
		.pal_burst  = 1,

		.composite_levels = &pal_n_levels_composite,
		.composite_color = &pal_n_csc_composite,
		.svideo_levels  = &pal_n_levels_svideo,
		.svideo_color = &pal_n_csc_svideo,

		.filter_table = tv_filter_table,
	},
	{
		/* 625 Lines, 50 Fields, 15.625KHz line, Sub-Carrier 4.434MHz */
		.name	    = "PAL",
		.clock		= 108000,
		.refresh	= 50000,
		.oversample	= 8,
		.component_only = 0,

		.hsync_end	= 64,		    .hblank_end		= 142,
		.hblank_start	= 844,	    .htotal		= 863,

		.progressive	= 0,    .trilevel_sync = 0,

		.vsync_start_f1	= 5,	    .vsync_start_f2	= 6,
		.vsync_len	= 5,

		.veq_ena	= 1,	    .veq_start_f1	= 0,
		.veq_start_f2	= 1,	    .veq_len		= 15,

		.vi_end_f1	= 24,		    .vi_end_f2		= 25,
		.nbr_end	= 286,

		.burst_ena	= 1,
		.hburst_start	= 73,		    .hburst_len		= 32,
		.vburst_start_f1 = 8,		    .vburst_end_f1	= 285,
		.vburst_start_f2 = 8,		    .vburst_end_f2	= 286,
		.vburst_start_f3 = 9,		    .vburst_end_f3	= 286,
		.vburst_start_f4 = 9,		    .vburst_end_f4	= 285,

		/* desired 4.4336180 actual 4.4336180 clock 107.52 */
		.dda1_inc       =    168,
		.dda2_inc       =   4122,       .dda2_size      =  27648,
		.dda3_inc       =     67,       .dda3_size      =    625,
		.sc_reset   = TV_SC_RESET_EVERY_8,
		.pal_burst  = 1,

		.composite_levels = &pal_levels_composite,
		.composite_color = &pal_csc_composite,
		.svideo_levels  = &pal_levels_svideo,
		.svideo_color = &pal_csc_svideo,

		.filter_table = tv_filter_table,
	},
	{
		.name       = "480p",
		.clock		= 108000,
		.refresh	= 59940,
		.oversample     = 4,
		.component_only = 1,

		.hsync_end      = 64,               .hblank_end         = 122,
		.hblank_start   = 842,              .htotal             = 857,

		.progressive    = 1,		    .trilevel_sync = 0,

		.vsync_start_f1 = 12,               .vsync_start_f2     = 12,
		.vsync_len      = 12,

		.veq_ena        = 0,

		.vi_end_f1      = 44,               .vi_end_f2          = 44,
		.nbr_end        = 479,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},
	{
		.name       = "576p",
		.clock		= 108000,
		.refresh	= 50000,
		.oversample     = 4,
		.component_only = 1,

		.hsync_end      = 64,               .hblank_end         = 139,
		.hblank_start   = 859,              .htotal             = 863,

		.progressive    = 1,		    .trilevel_sync = 0,

		.vsync_start_f1 = 10,               .vsync_start_f2     = 10,
		.vsync_len      = 10,

		.veq_ena        = 0,

		.vi_end_f1      = 48,               .vi_end_f2          = 48,
		.nbr_end        = 575,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},
	{
		.name       = "720p@60Hz",
		.clock		= 148500,
		.refresh	= 60000,
		.oversample     = 2,
		.component_only = 1,

		.hsync_end      = 80,               .hblank_end         = 300,
		.hblank_start   = 1580,             .htotal             = 1649,

		.progressive	= 1,		    .trilevel_sync = 1,

		.vsync_start_f1 = 10,               .vsync_start_f2     = 10,
		.vsync_len      = 10,

		.veq_ena        = 0,

		.vi_end_f1      = 29,               .vi_end_f2          = 29,
		.nbr_end        = 719,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},
	{
		.name       = "720p@50Hz",
		.clock		= 148500,
		.refresh	= 50000,
		.oversample     = 2,
		.component_only = 1,

		.hsync_end      = 80,               .hblank_end         = 300,
		.hblank_start   = 1580,             .htotal             = 1979,

		.progressive	= 1,		    .trilevel_sync = 1,

		.vsync_start_f1 = 10,               .vsync_start_f2     = 10,
		.vsync_len      = 10,

		.veq_ena        = 0,

		.vi_end_f1      = 29,               .vi_end_f2          = 29,
		.nbr_end        = 719,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},
	{
		.name       = "1080i@50Hz",
		.clock		= 148500,
		.refresh	= 50000,
		.oversample     = 2,
		.component_only = 1,

		.hsync_end      = 88,               .hblank_end         = 235,
		.hblank_start   = 2155,             .htotal             = 2639,

		.progressive	= 0,	  .trilevel_sync = 1,

		.vsync_start_f1 = 4,              .vsync_start_f2     = 5,
		.vsync_len      = 10,

		.veq_ena	= 1,	    .veq_start_f1	= 4,
		.veq_start_f2   = 4,	    .veq_len		= 10,


		.vi_end_f1      = 21,           .vi_end_f2          = 22,
		.nbr_end        = 539,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},
	{
		.name       = "1080i@60Hz",
		.clock		= 148500,
		.refresh	= 60000,
		.oversample     = 2,
		.component_only = 1,

		.hsync_end      = 88,               .hblank_end         = 235,
		.hblank_start   = 2155,             .htotal             = 2199,

		.progressive	= 0,	    .trilevel_sync = 1,

		.vsync_start_f1 = 4,               .vsync_start_f2     = 5,
		.vsync_len      = 10,

		.veq_ena	= 1,		    .veq_start_f1	= 4,
		.veq_start_f2	= 4,		    .veq_len		= 10,


		.vi_end_f1      = 21,               .vi_end_f2          = 22,
		.nbr_end        = 539,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},

	{
		.name       = "1080p@30Hz",
		.clock		= 148500,
		.refresh	= 30000,
		.oversample     = 2,
		.component_only = 1,

		.hsync_end      = 88,               .hblank_end         = 235,
		.hblank_start   = 2155,             .htotal             = 2199,

		.progressive	= 1,		    .trilevel_sync = 1,

		.vsync_start_f1 = 8,               .vsync_start_f2     = 8,
		.vsync_len      = 10,

		.veq_ena	= 0,	.veq_start_f1	= 0,
		.veq_start_f2	= 0,		    .veq_len		= 0,

		.vi_end_f1      = 44,               .vi_end_f2          = 44,
		.nbr_end        = 1079,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},

	{
		.name       = "1080p@50Hz",
		.clock		= 148500,
		.refresh	= 50000,
		.oversample     = 1,
		.component_only = 1,

		.hsync_end      = 88,               .hblank_end         = 235,
		.hblank_start   = 2155,             .htotal             = 2639,

		.progressive	= 1,		    .trilevel_sync = 1,

		.vsync_start_f1 = 8,               .vsync_start_f2     = 8,
		.vsync_len      = 10,

		.veq_ena	= 0,	.veq_start_f1	= 0,
		.veq_start_f2	= 0,		    .veq_len		= 0,

		.vi_end_f1      = 44,               .vi_end_f2          = 44,
		.nbr_end        = 1079,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},

	{
		.name       = "1080p@60Hz",
		.clock		= 148500,
		.refresh	= 60000,
		.oversample     = 1,
		.component_only = 1,

		.hsync_end      = 88,               .hblank_end         = 235,
		.hblank_start   = 2155,             .htotal             = 2199,

		.progressive	= 1,		    .trilevel_sync = 1,

		.vsync_start_f1 = 8,               .vsync_start_f2     = 8,
		.vsync_len      = 10,

		.veq_ena	= 0,		    .veq_start_f1	= 0,
		.veq_start_f2	= 0,		    .veq_len		= 0,

		.vi_end_f1      = 44,               .vi_end_f2          = 44,
		.nbr_end        = 1079,

		.burst_ena      = 0,

		.filter_table = tv_filter_table,
	},
};

/* The default format: NTSC-M. */
#define TV_FORMAT_DEFAULT 0

/* What the output keeps: the kind of connection the last detection
 * found, the format, and the margins (overscan) of the picture on the
 * set -- the values the video BIOS uses. */
struct tv_state {
	int type; /* DRM_MODE_CONNECTOR_Composite/SVIDEO/Component, Unknown */
	int format; /* index into tv_modes[] */
	int margin_left, margin_top, margin_right, margin_bottom;
};

/* A timing being worked on (signed, so the scaling arithmetic can be
 * done without casts). */
struct tv_timing {
	int clock;
	int hdisplay, hsync_start, hsync_end, htotal;
	int vdisplay, vsync_start, vsync_end, vtotal;
	uint32_t flags;
};

static const struct tv_mode *tv_mode_of(const struct lg_output *o)
{
	const struct tv_state *st = o->priv;

	return &tv_modes[st->format];
}

static int tv_mode_vdisplay(const struct tv_mode *tv_mode)
{
	if (tv_mode->progressive)
		return tv_mode->nbr_end + 1;
	else
		return 2 * (tv_mode->nbr_end + 1);
}

/* The timing a TV format amounts to at the pipe, with `clock' the TV
 * clock (the oversampled one). */
static void tv_mode_to_timing(struct tv_timing *t, const struct tv_mode *tv_mode,
			      int clock)
{
	t->clock = clock / (tv_mode->oversample >> !tv_mode->progressive);

	/*
	 * Horizontally the format counts from the start of hsync: hsync
	 * ends, then blanking ends (the active part starts), blanking
	 * starts again at hblank_start, and the line wraps at htotal.
	 */
	t->hdisplay = tv_mode->hblank_start - tv_mode->hblank_end;
	t->hsync_start = t->hdisplay + tv_mode->htotal - tv_mode->hblank_start;
	t->hsync_end = t->hsync_start + tv_mode->hsync_end;
	t->htotal = tv_mode->htotal + 1;

	/*
	 * Vertically: vsync, then the vertical interval up to vi_end, then
	 * the active lines up to nbr_end -- per field when interlaced.
	 */
	t->vdisplay = tv_mode_vdisplay(tv_mode);
	if (tv_mode->progressive) {
		t->vsync_start = t->vdisplay + tv_mode->vsync_start_f1 + 1;
		t->vsync_end = t->vsync_start + tv_mode->vsync_len;
		t->vtotal = t->vdisplay + tv_mode->vi_end_f1 + 1;
	} else {
		t->vsync_start = t->vdisplay + tv_mode->vsync_start_f1 + 1 +
				 tv_mode->vsync_start_f2 + 1;
		t->vsync_end = t->vsync_start + 2 * tv_mode->vsync_len;
		t->vtotal = t->vdisplay + tv_mode->vi_end_f1 + 1 +
			    tv_mode->vi_end_f2 + 1;
	}

	/* The encoder makes its own syncs: no sync or other flags. */
	t->flags = 0;
}

/* Scale the timing horizontally so that `hdisplay' pixels fill the
 * active area less the margins, keeping the line's duration. */
static void tv_scale_horiz(struct tv_timing *t, int hdisplay, int left_margin,
			   int right_margin)
{
	int hsync_start = t->hsync_start - t->hdisplay + right_margin;
	int hsync_end = t->hsync_end - t->hdisplay + right_margin;
	int new_htotal = (int)((int64_t)t->htotal * hdisplay /
			       (t->hdisplay - left_margin - right_margin));

	t->clock = (int)((int64_t)t->clock * new_htotal / t->htotal);

	t->hdisplay = hdisplay;
	t->hsync_start = hdisplay + (int)((int64_t)hsync_start * new_htotal / t->htotal);
	t->hsync_end = hdisplay + (int)((int64_t)hsync_end * new_htotal / t->htotal);
	t->htotal = new_htotal;
}

/* The same vertically, keeping the frame's duration. */
static void tv_scale_vert(struct tv_timing *t, int vdisplay, int top_margin,
			  int bottom_margin)
{
	int vsync_start = t->vsync_start - t->vdisplay + bottom_margin;
	int vsync_end = t->vsync_end - t->vdisplay + bottom_margin;
	int new_vtotal = (int)((int64_t)t->vtotal * vdisplay /
			       (t->vdisplay - top_margin - bottom_margin));

	t->clock = (int)((int64_t)t->clock * new_vtotal / t->vtotal);

	t->vdisplay = vdisplay;
	t->vsync_start = vdisplay + (int)((int64_t)vsync_start * new_vtotal / t->vtotal);
	t->vsync_end = vdisplay + (int)((int64_t)vsync_end * new_vtotal / t->vtotal);
	t->vtotal = new_vtotal;
}

/* The refresh rate of a mode in Hz, rounded. */
static uint32_t tv_mode_vrefresh(const struct drm_mode_modeinfo *m)
{
	uint64_t num, den;

	if (m->htotal == 0 || m->vtotal == 0)
		return 0;

	num = (uint64_t)m->clock * 1000;
	den = (uint64_t)m->htotal * m->vtotal;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (m->vscan > 1)
		den *= m->vscan;

	return (uint32_t)((num + den / 2) / den);
}

static void tv_timing_to_modeinfo(struct drm_mode_modeinfo *m, const struct tv_timing *t)
{
	mm_memset(m, 0, sizeof(*m));
	m->clock = (uint32_t)t->clock;
	m->hdisplay = (uint16_t)t->hdisplay;
	m->hsync_start = (uint16_t)t->hsync_start;
	m->hsync_end = (uint16_t)t->hsync_end;
	m->htotal = (uint16_t)t->htotal;
	m->vdisplay = (uint16_t)t->vdisplay;
	m->vsync_start = (uint16_t)t->vsync_start;
	m->vsync_end = (uint16_t)t->vsync_end;
	m->vtotal = (uint16_t)t->vtotal;
	m->flags = t->flags;
	m->vrefresh = tv_mode_vrefresh(m);
	ksnprintf(m->name, sizeof(m->name), "%dx%d%s", t->hdisplay, t->vdisplay,
		  (t->flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "");
}

/* ---- the pipe configuration ------------------------------------------------------- */

/* Gen3 cannot scale vertically a source wider than 1024 pixels. */
static int tv_source_too_wide(struct lg_display *d, int hdisplay)
{
	return d->ver == 3 && hdisplay > 1024;
}

static int tv_vert_scaling(const struct tv_timing *tv_t, const struct tv_state *st,
			   int vdisplay)
{
	return tv_t->vdisplay - st->margin_top - st->margin_bottom != vdisplay;
}

/*
 * The vertical margins and the vertical filter for a picture of
 * hdisplay x vdisplay on the format whose unscaled timing is `tv_t'.
 * When the vertical filter cannot be used (gen3 with a wide source) or is
 * not needed (the picture is exactly as tall as the area inside the
 * margins), it is bypassed and the picture centred in the extra lines,
 * keeping the margins' proportions.  A bypassed filter on an interlaced
 * format makes the pipe run interlaced at half the rate.
 */
static int tv_vmargins(struct lg_display *d, const struct tv_state *st,
		       const struct tv_mode *tv_mode, struct tv_timing *tv_t,
		       int hdisplay, int vdisplay, int *top_out, int *bottom_out,
		       int *bypass_vfilter)
{
	if (tv_source_too_wide(d, hdisplay) || !tv_vert_scaling(tv_t, st, vdisplay)) {
		int extra, top, bottom;

		extra = tv_t->vdisplay - vdisplay;

		if (extra < 0) {
			i915_dbg("[drm] i915: TV: no vertical scaling for >1024 pixel wide modes\n");
			return -EINVAL;
		}

		top = st->margin_top;
		bottom = st->margin_bottom;

		if (top + bottom)
			top = extra * top / (top + bottom);
		else
			top = extra / 2;
		bottom = extra - top;

		*top_out = top;
		*bottom_out = bottom;
		*bypass_vfilter = 1;

		if (!tv_mode->progressive) {
			tv_t->clock /= 2;
			tv_t->flags |= DRM_MODE_FLAG_INTERLACE;
		}
	} else {
		*top_out = st->margin_top;
		*bottom_out = st->margin_bottom;
		*bypass_vfilter = 0;
	}

	return 0;
}

/* The picture's size as the pipe's generator counts it. */
static void tv_client_size(const struct drm_mode_modeinfo *m, int *hdisplay, int *vdisplay)
{
	*hdisplay = m->hdisplay;
	*vdisplay = m->vdisplay;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		*vdisplay /= 2;
}

static int tv_mode_valid(struct lg_display *d, struct lg_output *o,
			 const struct drm_mode_modeinfo *m)
{
	const struct tv_mode *tv_mode = tv_mode_of(o);
	int refresh;

	/* The encoder takes a progressive, single-scanned picture. */
	if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN))
		return -EINVAL;

	if (d->max_dotclk_khz && m->clock > d->max_dotclk_khz)
		return -EINVAL;

	/* The refresh must be close to that of the TV format. */
	refresh = (int)tv_mode_vrefresh(m) * 1000;
	if (refresh - (int)tv_mode->refresh >= 1000 ||
	    (int)tv_mode->refresh - refresh >= 1000)
		return -EINVAL;

	return 0;
}

static int tv_compute_config(struct lg_display *d, struct lg_output *o,
			     struct lg_config *cfg)
{
	struct tv_state *st = o->priv;
	const struct tv_mode *tv_mode = tv_mode_of(o);
	struct tv_timing t;
	int hdisplay, vdisplay, top, bottom, bypass;

	if (cfg->mode.flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	tv_client_size(&cfg->mode, &hdisplay, &vdisplay);

	i915_dbg("[drm] i915: TV: forcing bpc to 8\n");
	cfg->pipe_bpp = 24;

	/* The PLL runs at the TV clock; the core finds its dividers. */
	cfg->port_clock = tv_mode->clock;

	tv_mode_to_timing(&t, tv_mode, (int)cfg->port_clock);

	if (tv_vmargins(d, st, tv_mode, &t, hdisplay, vdisplay, &top, &bottom, &bypass))
		return -EINVAL;

	i915_dbg("[drm] i915: TV mode %s: %d %d %d %d %d  %d %d %d %d  0x%x\n",
		 tv_mode->name, t.clock, t.hdisplay, t.hsync_start, t.hsync_end,
		 t.htotal, t.vdisplay, t.vsync_start, t.vsync_end, t.vtotal, t.flags);

	/*
	 * With the TV encoder the pipe wants to run faster than the TV: in
	 * the active part the encoder stalls it every few lines, in the
	 * bottom margin it stops, in the TV's vertical blank it runs free
	 * up to its vtotal and then waits for the TV to leave the top
	 * margin.  The pipe's timing is scaled as if it always ran at the
	 * average rate it keeps during the active part, which makes vtotal
	 * match that rate and gives a fair figure for the pixel rate; the
	 * scanline counter is somewhat off in the blank and the margins.
	 */
	tv_scale_horiz(&t, hdisplay, st->margin_left, st->margin_right);
	tv_scale_vert(&t, vdisplay, top, bottom);

	/* The generator's timing as the registers take it: blanking spans
	 * everything outside the active part.  Interlaced (a bypassed
	 * vertical filter) keeps the full vertical counts. */
	mm_memset(&cfg->t, 0, sizeof(cfg->t));
	cfg->t.clock = (uint32_t)t.clock;
	cfg->t.hdisplay = (uint16_t)t.hdisplay;
	cfg->t.hblank_start = (uint16_t)t.hdisplay;
	cfg->t.hsync_start = (uint16_t)t.hsync_start;
	cfg->t.hsync_end = (uint16_t)t.hsync_end;
	cfg->t.hblank_end = (uint16_t)t.htotal;
	cfg->t.htotal = (uint16_t)t.htotal;
	cfg->t.vdisplay = (uint16_t)t.vdisplay;
	cfg->t.vblank_start = (uint16_t)t.vdisplay;
	cfg->t.vsync_start = (uint16_t)t.vsync_start;
	cfg->t.vsync_end = (uint16_t)t.vsync_end;
	cfg->t.vblank_end = (uint16_t)t.vtotal;
	cfg->t.vtotal = (uint16_t)t.vtotal;
	cfg->t.flags = t.flags;
	cfg->t_set = 1;
	cfg->pixel_rate = (uint32_t)t.clock;

	/* The encoder scales on its own; the panel fitter stays off. */
	cfg->gmch_pfit.control = 0;
	cfg->gmch_pfit.pgm_ratios = 0;
	cfg->gmch_pfit.lvds_border_bits = 0;

	return 0;
}

/* ---- programming the encoder ------------------------------------------------------ */

static void tv_set_mode_timings(struct lg_display *d, const struct tv_mode *tv_mode,
				int burst_ena)
{
	uint32_t hctl1, hctl2, hctl3;
	uint32_t vctl1, vctl2, vctl3, vctl4, vctl5, vctl6, vctl7;

	hctl1 = ((uint32_t)tv_mode->hsync_end << TV_HSYNC_END_SHIFT) |
		((uint32_t)tv_mode->htotal << TV_HTOTAL_SHIFT);

	hctl2 = ((uint32_t)tv_mode->hburst_start << TV_HBURST_START_SHIFT) |
		((uint32_t)tv_mode->hburst_len << TV_HBURST_LEN_SHIFT);

	if (burst_ena)
		hctl2 |= TV_BURST_ENA;

	hctl3 = ((uint32_t)tv_mode->hblank_start << TV_HBLANK_START_SHIFT) |
		((uint32_t)tv_mode->hblank_end << TV_HBLANK_END_SHIFT);

	vctl1 = ((uint32_t)tv_mode->nbr_end << TV_NBR_END_SHIFT) |
		((uint32_t)tv_mode->vi_end_f1 << TV_VI_END_F1_SHIFT) |
		((uint32_t)tv_mode->vi_end_f2 << TV_VI_END_F2_SHIFT);

	vctl2 = ((uint32_t)tv_mode->vsync_len << TV_VSYNC_LEN_SHIFT) |
		((uint32_t)tv_mode->vsync_start_f1 << TV_VSYNC_START_F1_SHIFT) |
		((uint32_t)tv_mode->vsync_start_f2 << TV_VSYNC_START_F2_SHIFT);

	vctl3 = ((uint32_t)tv_mode->veq_len << TV_VEQ_LEN_SHIFT) |
		((uint32_t)tv_mode->veq_start_f1 << TV_VEQ_START_F1_SHIFT) |
		((uint32_t)tv_mode->veq_start_f2 << TV_VEQ_START_F2_SHIFT);

	if (tv_mode->veq_ena)
		vctl3 |= TV_EQUAL_ENA;

	vctl4 = ((uint32_t)tv_mode->vburst_start_f1 << TV_VBURST_START_F1_SHIFT) |
		((uint32_t)tv_mode->vburst_end_f1 << TV_VBURST_END_F1_SHIFT);

	vctl5 = ((uint32_t)tv_mode->vburst_start_f2 << TV_VBURST_START_F2_SHIFT) |
		((uint32_t)tv_mode->vburst_end_f2 << TV_VBURST_END_F2_SHIFT);

	vctl6 = ((uint32_t)tv_mode->vburst_start_f3 << TV_VBURST_START_F3_SHIFT) |
		((uint32_t)tv_mode->vburst_end_f3 << TV_VBURST_END_F3_SHIFT);

	vctl7 = ((uint32_t)tv_mode->vburst_start_f4 << TV_VBURST_START_F4_SHIFT) |
		((uint32_t)tv_mode->vburst_end_f4 << TV_VBURST_END_F4_SHIFT);

	lg_wr(d, TV_H_CTL_1, hctl1);
	lg_wr(d, TV_H_CTL_2, hctl2);
	lg_wr(d, TV_H_CTL_3, hctl3);
	lg_wr(d, TV_V_CTL_1, vctl1);
	lg_wr(d, TV_V_CTL_2, vctl2);
	lg_wr(d, TV_V_CTL_3, vctl3);
	lg_wr(d, TV_V_CTL_4, vctl4);
	lg_wr(d, TV_V_CTL_5, vctl5);
	lg_wr(d, TV_V_CTL_6, vctl6);
	lg_wr(d, TV_V_CTL_7, vctl7);
}

static void tv_set_color_conversion(struct lg_display *d,
				    const struct tv_color_conversion *cc)
{
	lg_wr(d, TV_CSC_Y, ((uint32_t)cc->ry << 16) | cc->gy);
	lg_wr(d, TV_CSC_Y2, ((uint32_t)cc->by << 16) | cc->ay);
	lg_wr(d, TV_CSC_U, ((uint32_t)cc->ru << 16) | cc->gu);
	lg_wr(d, TV_CSC_U2, ((uint32_t)cc->bu << 16) | cc->au);
	lg_wr(d, TV_CSC_V, ((uint32_t)cc->rv << 16) | cc->gv);
	lg_wr(d, TV_CSC_V2, ((uint32_t)cc->bv << 16) | cc->av);
}

/* With the pipe and its PLL still off: the whole encoder for the format
 * and the connection, the encoder itself left off. */
static void tv_pre_enable(struct lg_display *d, struct lg_output *o,
			  const struct lg_config *cfg)
{
	struct tv_state *st = o->priv;
	const struct tv_mode *tv_mode = tv_mode_of(o);
	const struct tv_video_levels *video_levels;
	const struct tv_color_conversion *color_conversion;
	struct tv_timing tv_t;
	uint32_t tv_ctl, tv_filter_ctl;
	uint32_t scctl1, scctl2, scctl3;
	int hdisplay, vdisplay, top, bottom, bypass_vfilter;
	int burst_ena;
	int i, j;
	int xpos, ypos;
	unsigned int xsize, ysize;

	/* The margins and the vertical filter as compute_config chose them. */
	tv_client_size(&cfg->mode, &hdisplay, &vdisplay);
	tv_mode_to_timing(&tv_t, tv_mode, (int)tv_mode->clock);
	if (tv_vmargins(d, st, tv_mode, &tv_t, hdisplay, vdisplay, &top, &bottom,
			&bypass_vfilter)) {
		top = st->margin_top;
		bottom = st->margin_bottom;
		bypass_vfilter = 1;
	}

	tv_ctl = lg_rd(d, TV_CTL);
	tv_ctl &= TV_CTL_SAVE;

	switch (st->type) {
	default:
	case DRM_MODE_CONNECTOR_Unknown:
	case DRM_MODE_CONNECTOR_Composite:
		tv_ctl |= TV_ENC_OUTPUT_COMPOSITE;
		video_levels = tv_mode->composite_levels;
		color_conversion = tv_mode->composite_color;
		burst_ena = tv_mode->burst_ena;
		break;
	case DRM_MODE_CONNECTOR_Component:
		tv_ctl |= TV_ENC_OUTPUT_COMPONENT;
		video_levels = &component_levels;
		if (tv_mode->burst_ena)
			color_conversion = &sdtv_csc_yprpb;
		else
			color_conversion = &hdtv_csc_yprpb;
		burst_ena = 0;
		break;
	case DRM_MODE_CONNECTOR_SVIDEO:
		tv_ctl |= TV_ENC_OUTPUT_SVIDEO;
		video_levels = tv_mode->svideo_levels;
		color_conversion = tv_mode->svideo_color;
		burst_ena = tv_mode->burst_ena;
		break;
	}

	/* The component-only formats carry no composite/S-video tables:
	 * they get the component matrix whatever was detected. */
	if (!color_conversion)
		color_conversion = &hdtv_csc_yprpb;

	tv_ctl |= TV_ENC_PIPE_SEL(cfg->pipe);

	switch (tv_mode->oversample) {
	case 8:
		tv_ctl |= TV_OVERSAMPLE_8X;
		break;
	case 4:
		tv_ctl |= TV_OVERSAMPLE_4X;
		break;
	case 2:
		tv_ctl |= TV_OVERSAMPLE_2X;
		break;
	default:
		tv_ctl |= TV_OVERSAMPLE_NONE;
		break;
	}

	if (tv_mode->progressive)
		tv_ctl |= TV_PROGRESSIVE;
	if (tv_mode->trilevel_sync)
		tv_ctl |= TV_TRILEVEL_SYNC;
	if (tv_mode->pal_burst)
		tv_ctl |= TV_PAL_BURST;

	scctl1 = 0;
	if (tv_mode->dda1_inc)
		scctl1 |= TV_SC_DDA1_EN;
	if (tv_mode->dda2_inc)
		scctl1 |= TV_SC_DDA2_EN;
	if (tv_mode->dda3_inc)
		scctl1 |= TV_SC_DDA3_EN;
	scctl1 |= tv_mode->sc_reset;
	if (video_levels)
		scctl1 |= (uint32_t)video_levels->burst << TV_BURST_LEVEL_SHIFT;
	scctl1 |= (uint32_t)tv_mode->dda1_inc << TV_SCDDA1_INC_SHIFT;

	scctl2 = (uint32_t)tv_mode->dda2_size << TV_SCDDA2_SIZE_SHIFT |
		 (uint32_t)tv_mode->dda2_inc << TV_SCDDA2_INC_SHIFT;

	scctl3 = (uint32_t)tv_mode->dda3_size << TV_SCDDA3_SIZE_SHIFT |
		 (uint32_t)tv_mode->dda3_inc << TV_SCDDA3_INC_SHIFT;

	/* The two fixes the 915GM needs. */
	if (d->is_i915gm)
		tv_ctl |= TV_ENC_C0_FIX | TV_ENC_SDP_FIX;

	tv_set_mode_timings(d, tv_mode, burst_ena);

	lg_wr(d, TV_SC_CTL_1, scctl1);
	lg_wr(d, TV_SC_CTL_2, scctl2);
	lg_wr(d, TV_SC_CTL_3, scctl3);

	tv_set_color_conversion(d, color_conversion);

	/* Brightness, contrast, saturation and hue at their neutral values. */
	if (d->ver >= 4)
		lg_wr(d, TV_CLR_KNOBS, 0x00404000);
	else
		lg_wr(d, TV_CLR_KNOBS, 0x00606000);

	if (video_levels)
		lg_wr(d, TV_CLR_LEVEL,
		      ((uint32_t)video_levels->black << TV_BLACK_LEVEL_SHIFT) |
		      ((uint32_t)video_levels->blank << TV_BLANK_LEVEL_SHIFT));

	/* The filter control must be written before TV_WIN_SIZE. */
	tv_filter_ctl = TV_AUTO_SCALE;
	if (bypass_vfilter)
		tv_filter_ctl |= TV_V_FILTER_BYPASS;
	lg_wr(d, TV_FILTER_CTL_1, tv_filter_ctl);

	xsize = (unsigned int)(tv_mode->hblank_start - tv_mode->hblank_end);
	ysize = (unsigned int)tv_mode_vdisplay(tv_mode);

	xpos = st->margin_left;
	ypos = top;
	xsize -= (unsigned int)(st->margin_left + st->margin_right);
	ysize -= (unsigned int)(top + bottom);
	lg_wr(d, TV_WIN_POS, ((uint32_t)xpos << 16) | (uint32_t)ypos);
	lg_wr(d, TV_WIN_SIZE, ((uint32_t)xsize << 16) | ysize);

	j = 0;
	for (i = 0; i < 60; i++)
		lg_wr(d, TV_H_LUMA(i), tv_mode->filter_table[j++]);
	for (i = 0; i < 60; i++)
		lg_wr(d, TV_H_CHROMA(i), tv_mode->filter_table[j++]);
	for (i = 0; i < 43; i++)
		lg_wr(d, TV_V_LUMA(i), tv_mode->filter_table[j++]);
	for (i = 0; i < 43; i++)
		lg_wr(d, TV_V_CHROMA(i), tv_mode->filter_table[j++]);
	lg_wr(d, TV_DAC, lg_rd(d, TV_DAC) & TV_DAC_SAVE);
	lg_wr(d, TV_CTL, tv_ctl);

	i915_dbg("[drm] i915: TV: %s on pipe %c, %s, window %d,%d %ux%u%s\n",
		 tv_mode->name, 'A' + cfg->pipe,
		 st->type == DRM_MODE_CONNECTOR_Component ? "component" :
		 st->type == DRM_MODE_CONNECTOR_SVIDEO ? "S-video" : "composite",
		 xpos, ypos, xsize, ysize, bypass_vfilter ? ", no vertical filter" : "");
}

static void tv_enable(struct lg_display *d, struct lg_output *o,
		      const struct lg_config *cfg)
{
	(void)o;

	/* A frame of the running pipe first, or the vblank waits of a later
	 * load detection time out. */
	lg_wait_for_vblank(d, cfg->pipe);

	lg_rmw(d, TV_CTL, 0, TV_ENC_ENABLE);
}

static void tv_disable(struct lg_display *d, struct lg_output *o,
		       const struct lg_config *cfg)
{
	(void)o;
	(void)cfg;

	lg_rmw(d, TV_CTL, TV_ENC_ENABLE, 0);
}

static int tv_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	uint32_t tmp = lg_rd(d, TV_CTL);

	(void)o;
	*pipe = (int)((tmp & TV_ENC_PIPE_SEL_MASK) >> TV_ENC_PIPE_SEL_SHIFT);

	return !!(tmp & TV_ENC_ENABLE);
}

/* ---- detection and modes ------------------------------------------------------------ */

/* The size the pipe is given for a load detection (the first of the
 * input sizes the modes offer). */
static void tv_load_detect_mode(const struct tv_mode *tv_mode, struct drm_mode_modeinfo *m)
{
	struct tv_timing t;

	tv_mode_to_timing(&t, tv_mode, (int)tv_mode->clock);
	tv_scale_horiz(&t, 640, 0, 0);
	tv_scale_vert(&t, 480, 0, 0);
	tv_timing_to_modeinfo(m, &t);
	m->type = DRM_MODE_TYPE_DRIVER;
}

/*
 * Sense what hangs on the DACs, with `pipe' running: the encoder off and
 * in its monitor-detect test mode drives the three DACs at half level,
 * and the state change logic reports which of them see a load.  Returns
 * the connector type found, or -1.
 */
static int tv_detect_type(struct lg_display *d, int pipe)
{
	uint32_t tv_ctl, save_tv_ctl;
	uint32_t tv_dac, save_tv_dac;
	int type;

	save_tv_dac = tv_dac = lg_rd(d, TV_DAC);
	save_tv_ctl = tv_ctl = lg_rd(d, TV_CTL);

	/* Poll for TV detection */
	tv_ctl &= ~(TV_ENC_ENABLE | TV_ENC_PIPE_SEL_MASK | TV_TEST_MODE_MASK);
	tv_ctl |= TV_TEST_MODE_MONITOR_DETECT;
	tv_ctl |= TV_ENC_PIPE_SEL(pipe);

	tv_dac &= ~(TVDAC_SENSE_MASK | TV_DAC_A_MASK | TV_DAC_B_MASK | TV_DAC_C_MASK);
	tv_dac |= (TVDAC_STATE_CHG_EN |
		   TVDAC_A_SENSE_CTL |
		   TVDAC_B_SENSE_CTL |
		   TVDAC_C_SENSE_CTL |
		   TV_DAC_CTL_OVERRIDE |
		   TV_DAC_A_0_7_V |
		   TV_DAC_B_0_7_V |
		   TV_DAC_C_0_7_V);

	/* GM45 misdetects the TV unless the sense state is left clear. */
	if (d->is_gm45)
		tv_dac &= ~(TVDAC_STATE_CHG_EN | TVDAC_A_SENSE_CTL |
			    TVDAC_B_SENSE_CTL | TVDAC_C_SENSE_CTL);

	lg_wr(d, TV_CTL, tv_ctl);
	lg_wr(d, TV_DAC, tv_dac);
	lg_posting_read(d, TV_DAC);

	lg_wait_for_vblank(d, pipe);

	type = -1;
	tv_dac = lg_rd(d, TV_DAC);
	i915_dbg("[drm] i915: TV detected: %x, %x\n", tv_ctl, tv_dac);
	/*
	 *  A B C
	 *  0 1 1 Composite
	 *  1 0 X S-video
	 *  0 0 0 Component
	 */
	if ((tv_dac & TVDAC_SENSE_MASK) == (TVDAC_B_SENSE | TVDAC_C_SENSE)) {
		i915_dbg("[drm] i915: TV: composite connection\n");
		type = DRM_MODE_CONNECTOR_Composite;
	} else if ((tv_dac & (TVDAC_A_SENSE | TVDAC_B_SENSE)) == TVDAC_A_SENSE) {
		i915_dbg("[drm] i915: TV: S-video connection\n");
		type = DRM_MODE_CONNECTOR_SVIDEO;
	} else if ((tv_dac & TVDAC_SENSE_MASK) == 0) {
		i915_dbg("[drm] i915: TV: component connection\n");
		type = DRM_MODE_CONNECTOR_Component;
	} else {
		i915_dbg("[drm] i915: TV: unrecognised connection\n");
		type = -1;
	}

	lg_wr(d, TV_DAC, save_tv_dac & ~TVDAC_STATE_CHG_EN);
	lg_wr(d, TV_CTL, save_tv_ctl);
	lg_posting_read(d, TV_CTL);

	/* The hardware misbehaves without this wait. */
	lg_wait_for_vblank(d, pipe);

	return type;
}

/* A component-only format on a composite or S-video connection makes no
 * picture: fall back to the first format that is not component-only. */
static void tv_find_better_format(struct tv_state *st)
{
	int i;

	/* Component takes every format. */
	if (st->type == DRM_MODE_CONNECTOR_Component)
		return;

	/* The current format is fine. */
	if (!tv_modes[st->format].component_only)
		return;

	for (i = 0; i < TV_NELEM(tv_modes); i++) {
		if (!tv_modes[i].component_only)
			break;
	}

	st->format = i < TV_NELEM(tv_modes) ? i : TV_FORMAT_DEFAULT;
}

static int tv_detect(struct lg_display *d, struct lg_output *o)
{
	struct tv_state *st = o->priv;
	struct drm_mode_modeinfo mode;
	int pipe, type;

	tv_load_detect_mode(tv_mode_of(o), &mode);

	pipe = lg_load_detect_get(d, o, &mode);
	if (pipe < 0) {
		i915_dbg("[drm] i915: TV: no pipe for load detection (%d)\n", pipe);
		return o->detected;
	}

	type = tv_detect_type(d, pipe);
	lg_load_detect_release(d, o);

	if (type < 0)
		return 0;

	st->type = type;
	tv_find_better_format(st);
	return 1;
}

/* The picture sizes offered on the TV. */
static const struct tv_input_res {
	uint16_t w, h;
} tv_input_res_table[] = {
	{ 640, 480 },
	{ 800, 600 },
	{ 1024, 768 },
	{ 1280, 1024 },
	{ 848, 480 },
	{ 1280, 720 },
	{ 1920, 1080 },
};

/* The preferred size goes by the format's lines: 480 for every SD format. */
static int tv_is_preferred_mode(const struct drm_mode_modeinfo *m,
				const struct tv_mode *tv_mode)
{
	int vdisplay = tv_mode_vdisplay(tv_mode);

	if (vdisplay <= 576)
		vdisplay = 480;

	return vdisplay == m->vdisplay;
}

static int tv_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	const struct tv_mode *tv_mode = tv_mode_of(o);
	int i, count = 0;

	for (i = 0; i < TV_NELEM(tv_input_res_table); i++) {
		const struct tv_input_res *input = &tv_input_res_table[i];
		struct drm_mode_modeinfo m;
		struct tv_timing t;

		if (input->w > 1024 && !tv_mode->progressive && !tv_mode->component_only)
			continue;

		/* no vertical scaling with wide sources on gen3 */
		if (d->ver == 3 && input->w > 1024 && input->h > tv_mode_vdisplay(tv_mode))
			continue;

		/* The TV format scaled to look as if it had this size: the
		 * truest picture of the actual timing, margins aside. */
		tv_mode_to_timing(&t, tv_mode, (int)tv_mode->clock);
		if (count == 0)
			i915_dbg("[drm] i915: TV mode %s: %d %d %d %d %d  %d %d %d %d\n",
				 tv_mode->name, t.clock, t.hdisplay, t.hsync_start,
				 t.hsync_end, t.htotal, t.vdisplay, t.vsync_start,
				 t.vsync_end, t.vtotal);
		tv_scale_horiz(&t, input->w, 0, 0);
		tv_scale_vert(&t, input->h, 0, 0);
		tv_timing_to_modeinfo(&m, &t);

		m.type = DRM_MODE_TYPE_DRIVER;
		if (tv_is_preferred_mode(&m, tv_mode))
			m.type |= DRM_MODE_TYPE_PREFERRED;

		if (drm_connector_add_mode(d->drm, conn, &m) < 0)
			continue;
		count++;
	}

	return count;
}

static const struct lg_output_funcs tv_funcs = {
	.detect = tv_detect,
	.get_modes = tv_get_modes,
	.mode_valid = tv_mode_valid,
	.compute_config = tv_compute_config,
	.pre_enable = tv_pre_enable,
	.enable = tv_enable,
	.disable = tv_disable,
	.get_hw_state = tv_get_hw_state,
};

/* ---- init --------------------------------------------------------------------------- */

void lg_tv_init(struct lg_display *d)
{
	struct lg_output *o;
	struct tv_state *st;
	uint32_t tv_dac_on, tv_dac_off, save_tv_dac;

	if (!(d->is_i915gm || d->is_i945gm || d->is_i965gm || d->is_gm45))
		return;

	if ((lg_rd(d, TV_CTL) & TV_FUSE_STATE_MASK) == TV_FUSE_STATE_DISABLED)
		return;

	if (!d->vbt.int_tv_support || !lg_vbt_tv_present(d)) {
		i915_dbg("[drm] i915: integrated TV is not present\n");
		return;
	}

	/* Is the DAC register really there?  The state change enable bit
	 * must read back as written, both ways. */
	save_tv_dac = lg_rd(d, TV_DAC);

	lg_wr(d, TV_DAC, save_tv_dac | TVDAC_STATE_CHG_EN);
	tv_dac_on = lg_rd(d, TV_DAC);

	lg_wr(d, TV_DAC, save_tv_dac & ~TVDAC_STATE_CHG_EN);
	tv_dac_off = lg_rd(d, TV_DAC);

	lg_wr(d, TV_DAC, save_tv_dac);

	if ((tv_dac_on & TVDAC_STATE_CHG_EN) == 0 ||
	    (tv_dac_off & TVDAC_STATE_CHG_EN) != 0)
		return;

	st = kalloc(sizeof(*st));
	if (!st)
		return;
	mm_memset(st, 0, sizeof(*st));

	o = lg_output_new(d);
	if (!o) {
		kfree(st);
		return;
	}

	st->type = DRM_MODE_CONNECTOR_Unknown;
	st->format = TV_FORMAT_DEFAULT;
	/* the margins the video BIOS uses */
	st->margin_left = 54;
	st->margin_top = 36;
	st->margin_right = 46;
	st->margin_bottom = 37;

	o->type = LG_OUTPUT_TVOUT;
	o->port = -1;
	o->reg = TV_CTL;
	ksnprintf(o->name, sizeof(o->name), "TV");
	o->funcs = &tv_funcs;
	o->conn_type = DRM_MODE_CONNECTOR_SVIDEO;
	o->enc_type = DRM_MODE_ENCODER_TVDAC;
	o->pipe_mask = (1u << d->num_pipes) - 1;
	/*
	 * Polled rather than hotplugged: the TV hotplug detection needs the
	 * TV's PLL kept running, which costs power and bandwidth and makes
	 * the pipes underrun on Crestline while the encoder sits idle.
	 */
	o->hpd_pin = LG_HPD_NONE;
	o->polled = 1;
	o->ddc_pin = LG_GMBUS_PIN_DISABLED;
	o->ddc = NULL;
	o->priv = st;

	kprintf("[drm] i915: TV-out (%s)\n", tv_modes[st->format].name);
}
