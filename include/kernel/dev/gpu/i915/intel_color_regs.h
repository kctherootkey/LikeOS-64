// LikeOS -- the colour registers of the Intel display pipes (Haswell
// on): the gamma mode, the legacy and precision palettes with their
// index/data ports, the pre-CSC (degamma) table of Gemini Lake on, the
// multi-segmented gamma of Ice Lake and Tiger Lake, the pipe CSC and the
// output CSC, and the pipe bottom colour.
//
// Each pipe's copy sits at the pipe A address plus the pipe's index
// times the distance to pipe B's (pipe C and D continue the series).
// The legacy palette, GAMMA_MODE, SKL_BOTTOM_COLOR and the plane enable
// bits are in i915_reg.h.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2023 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_COLOR_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_COLOR_REGS_H

#include <kernel/dev/gpu/i915/i915_reg.h>

/* pipe A's register plus `pipe' times the distance to pipe B's */
#define _COLOR_PIPE(pipe, a, b) ((uint32_t)(a) + (uint32_t)(pipe) * ((uint32_t)(b) - (uint32_t)(a)))

/* ---- value formats ------------------------------------------------------------- */

/* the 8-bit palette (LGC_PALETTE): 8:8:8 */
#define PALETTE_RED_MASK GENMASK(23, 16)
#define PALETTE_GREEN_MASK GENMASK(15, 8)
#define PALETTE_BLUE_MASK GENMASK(7, 0)

/* the precision palette, 10-bit mode: 10:10:10 */
#define PREC_PALETTE_10_RED_MASK GENMASK(29, 20)
#define PREC_PALETTE_10_GREEN_MASK GENMASK(19, 10)
#define PREC_PALETTE_10_BLUE_MASK GENMASK(9, 0)
/* 12.4 interpolated mode, the even dword: the low 6 bits of each channel */
#define PREC_PALETTE_12P4_RED_LDW_MASK GENMASK(29, 24)
#define PREC_PALETTE_12P4_GREEN_LDW_MASK GENMASK(19, 14)
#define PREC_PALETTE_12P4_BLUE_LDW_MASK GENMASK(9, 4)
/* ...and the odd dword: the high 10 bits */
#define PREC_PALETTE_12P4_RED_UDW_MASK GENMASK(29, 20)
#define PREC_PALETTE_12P4_GREEN_UDW_MASK GENMASK(19, 10)
#define PREC_PALETTE_12P4_BLUE_UDW_MASK GENMASK(9, 0)

/* ---- GAMMA_MODE (the register itself and modes 0..3 are in i915_reg.h) ---------- */

#define PRE_CSC_GAMMA_ENABLE (1u << 31) /* Ice Lake on */
#define POST_CSC_GAMMA_ENABLE (1u << 30) /* Ice Lake on */
#define PALETTE_ANTICOL_DISABLE (1u << 15) /* Skylake on */
#define GAMMA_MODE_MODE_MASK GENMASK(1, 0)
/* mode 3: split on Haswell..Skylake, multi-segmented on Ice Lake/Tiger Lake */
#define GAMMA_MODE_MODE_12BIT_MULTI_SEG 3

/* ---- the pipe CSC ----------------------------------------------------------- */

#define _PIPE_A_CSC_COEFF_RY_GY 0x49010
#define _PIPE_A_CSC_COEFF_BY 0x49014
#define _PIPE_A_CSC_COEFF_RU_GU 0x49018
#define _PIPE_A_CSC_COEFF_BU 0x4901c
#define _PIPE_A_CSC_COEFF_RV_GV 0x49020
#define _PIPE_A_CSC_COEFF_BV 0x49024
#define _PIPE_A_CSC_MODE 0x49028
#define _PIPE_A_CSC_PREOFF_HI 0x49030
#define _PIPE_A_CSC_PREOFF_ME 0x49034
#define _PIPE_A_CSC_PREOFF_LO 0x49038
#define _PIPE_A_CSC_POSTOFF_HI 0x49040
#define _PIPE_A_CSC_POSTOFF_ME 0x49044
#define _PIPE_A_CSC_POSTOFF_LO 0x49048
#define _PIPE_CSC_STRIDE 0x100

#define _PIPE_CSC(pipe, a) ((uint32_t)(a) + (uint32_t)(pipe) * _PIPE_CSC_STRIDE)
#define PIPE_CSC_COEFF_RY_GY(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_COEFF_RY_GY)
#define PIPE_CSC_COEFF_BY(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_COEFF_BY)
#define PIPE_CSC_COEFF_RU_GU(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_COEFF_RU_GU)
#define PIPE_CSC_COEFF_BU(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_COEFF_BU)
#define PIPE_CSC_COEFF_RV_GV(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_COEFF_RV_GV)
#define PIPE_CSC_COEFF_BV(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_COEFF_BV)
#define PIPE_CSC_MODE(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_MODE)
#define PIPE_CSC_PREOFF_HI(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_PREOFF_HI)
#define PIPE_CSC_PREOFF_ME(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_PREOFF_ME)
#define PIPE_CSC_PREOFF_LO(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_PREOFF_LO)
#define PIPE_CSC_POSTOFF_HI(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_POSTOFF_HI)
#define PIPE_CSC_POSTOFF_ME(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_POSTOFF_ME)
#define PIPE_CSC_POSTOFF_LO(pipe) _PIPE_CSC(pipe, _PIPE_A_CSC_POSTOFF_LO)

/* CSC_MODE */
#define ICL_CSC_ENABLE (1u << 31) /* Ice Lake on */
#define ICL_OUTPUT_CSC_ENABLE (1u << 30) /* Ice Lake on */
#define CSC_BLACK_SCREEN_OFFSET (1u << 2) /* Ironlake/Sandy Bridge */
#define CSC_POSITION_BEFORE_GAMMA (1u << 1) /* before Gemini Lake */
#define CSC_MODE_YUV_TO_RGB (1u << 0) /* Ironlake/Sandy Bridge */

/* ---- the output CSC (Ice Lake on) ----------------------------------------------- */

#define _PIPE_A_OUTPUT_CSC_COEFF_RY_GY 0x49050
#define _PIPE_A_OUTPUT_CSC_COEFF_BY 0x49054
#define _PIPE_A_OUTPUT_CSC_COEFF_RU_GU 0x49058
#define _PIPE_A_OUTPUT_CSC_COEFF_BU 0x4905c
#define _PIPE_A_OUTPUT_CSC_COEFF_RV_GV 0x49060
#define _PIPE_A_OUTPUT_CSC_COEFF_BV 0x49064
#define _PIPE_A_OUTPUT_CSC_PREOFF_HI 0x49068
#define _PIPE_A_OUTPUT_CSC_PREOFF_ME 0x4906c
#define _PIPE_A_OUTPUT_CSC_PREOFF_LO 0x49070
#define _PIPE_A_OUTPUT_CSC_POSTOFF_HI 0x49074
#define _PIPE_A_OUTPUT_CSC_POSTOFF_ME 0x49078
#define _PIPE_A_OUTPUT_CSC_POSTOFF_LO 0x4907c

#define PIPE_CSC_OUTPUT_COEFF_RY_GY(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_COEFF_RY_GY)
#define PIPE_CSC_OUTPUT_COEFF_BY(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_COEFF_BY)
#define PIPE_CSC_OUTPUT_COEFF_RU_GU(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_COEFF_RU_GU)
#define PIPE_CSC_OUTPUT_COEFF_BU(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_COEFF_BU)
#define PIPE_CSC_OUTPUT_COEFF_RV_GV(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_COEFF_RV_GV)
#define PIPE_CSC_OUTPUT_COEFF_BV(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_COEFF_BV)
#define PIPE_CSC_OUTPUT_PREOFF_HI(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_PREOFF_HI)
#define PIPE_CSC_OUTPUT_PREOFF_ME(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_PREOFF_ME)
#define PIPE_CSC_OUTPUT_PREOFF_LO(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_PREOFF_LO)
#define PIPE_CSC_OUTPUT_POSTOFF_HI(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_POSTOFF_HI)
#define PIPE_CSC_OUTPUT_POSTOFF_ME(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_POSTOFF_ME)
#define PIPE_CSC_OUTPUT_POSTOFF_LO(pipe) _PIPE_CSC(pipe, _PIPE_A_OUTPUT_CSC_POSTOFF_LO)

/* ---- the precision palette (Haswell on): degamma and gamma ----------------------- */

#define _PAL_PREC_INDEX_A 0x4a400
#define _PAL_PREC_INDEX_B 0x4ac00
#define PAL_PREC_SPLIT_MODE (1u << 31)
#define PAL_PREC_AUTO_INCREMENT (1u << 15)
#define PAL_PREC_INDEX_VALUE_MASK GENMASK(9, 0)
#define PAL_PREC_INDEX_VALUE(x) FIELD_PREP(PAL_PREC_INDEX_VALUE_MASK, (x))
#define _PAL_PREC_DATA_A 0x4a404
#define _PAL_PREC_DATA_B 0x4ac04
#define _PAL_PREC_GC_MAX_A 0x4a410
#define _PAL_PREC_GC_MAX_B 0x4ac10
#define _PAL_PREC_EXT_GC_MAX_A 0x4a420
#define _PAL_PREC_EXT_GC_MAX_B 0x4ac20
#define _PAL_PREC_EXT2_GC_MAX_A 0x4a430
#define _PAL_PREC_EXT2_GC_MAX_B 0x4ac30

#define PREC_PAL_INDEX(pipe) _COLOR_PIPE(pipe, _PAL_PREC_INDEX_A, _PAL_PREC_INDEX_B)
#define PREC_PAL_DATA(pipe) _COLOR_PIPE(pipe, _PAL_PREC_DATA_A, _PAL_PREC_DATA_B)
/* u1.16 */
#define PREC_PAL_GC_MAX(pipe, i) (_COLOR_PIPE(pipe, _PAL_PREC_GC_MAX_A, _PAL_PREC_GC_MAX_B) + (i) * 4)
/* u3.16 */
#define PREC_PAL_EXT_GC_MAX(pipe, i) \
	(_COLOR_PIPE(pipe, _PAL_PREC_EXT_GC_MAX_A, _PAL_PREC_EXT_GC_MAX_B) + (i) * 4)
/* Gemini Lake on, u3.16 */
#define PREC_PAL_EXT2_GC_MAX(pipe, i) \
	(_COLOR_PIPE(pipe, _PAL_PREC_EXT2_GC_MAX_A, _PAL_PREC_EXT2_GC_MAX_B) + (i) * 4)

/* the pre-CSC (degamma) table, Gemini Lake on */
#define _PRE_CSC_GAMC_INDEX_A 0x4a484
#define _PRE_CSC_GAMC_INDEX_B 0x4ac84
#define PRE_CSC_GAMC_AUTO_INCREMENT (1u << 10)
#define PRE_CSC_GAMC_INDEX_VALUE_MASK GENMASK(7, 0)
#define PRE_CSC_GAMC_INDEX_VALUE(x) FIELD_PREP(PRE_CSC_GAMC_INDEX_VALUE_MASK, (x))
#define _PRE_CSC_GAMC_DATA_A 0x4a488
#define _PRE_CSC_GAMC_DATA_B 0x4ac88

#define PRE_CSC_GAMC_INDEX(pipe) _COLOR_PIPE(pipe, _PRE_CSC_GAMC_INDEX_A, _PRE_CSC_GAMC_INDEX_B)
#define PRE_CSC_GAMC_DATA(pipe) _COLOR_PIPE(pipe, _PRE_CSC_GAMC_DATA_A, _PRE_CSC_GAMC_DATA_B)

/* the multi-segmented gamma's super-fine segment, Ice Lake / Tiger Lake */
#define _PAL_PREC_MULTI_SEG_INDEX_A 0x4a408
#define _PAL_PREC_MULTI_SEG_INDEX_B 0x4ac08
#define PAL_PREC_MULTI_SEG_AUTO_INCREMENT (1u << 15)
#define PAL_PREC_MULTI_SEG_INDEX_VALUE_MASK GENMASK(4, 0)
#define PAL_PREC_MULTI_SEG_INDEX_VALUE(x) FIELD_PREP(PAL_PREC_MULTI_SEG_INDEX_VALUE_MASK, (x))
#define _PAL_PREC_MULTI_SEG_DATA_A 0x4a40c
#define _PAL_PREC_MULTI_SEG_DATA_B 0x4ac0c

#define PREC_PAL_MULTI_SEG_INDEX(pipe) \
	_COLOR_PIPE(pipe, _PAL_PREC_MULTI_SEG_INDEX_A, _PAL_PREC_MULTI_SEG_INDEX_B)
#define PREC_PAL_MULTI_SEG_DATA(pipe) \
	_COLOR_PIPE(pipe, _PAL_PREC_MULTI_SEG_DATA_A, _PAL_PREC_MULTI_SEG_DATA_B)

/* ---- the pipe bottom colour (SKL_BOTTOM_COLOR is in i915_reg.h) ---------------- */

#define SKL_BOTTOM_COLOR_GAMMA_ENABLE (1u << 31)
#define SKL_BOTTOM_COLOR_CSC_ENABLE (1u << 30)

/* ---- the planes' pipe CSC enables (the gamma enables are in i915_reg.h) -------- */

#ifndef DISP_PIPE_CSC_ENABLE
#define DISP_PIPE_CSC_ENABLE (1u << 24) /* DSPCNTR, Ironlake on */
#endif
#ifndef MCURSOR_PIPE_CSC_ENABLE
#define MCURSOR_PIPE_CSC_ENABLE (1u << 24) /* CUR_CTL, Ironlake on */
#endif

#endif
