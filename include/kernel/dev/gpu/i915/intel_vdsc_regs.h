// LikeOS -- registers of the display stream compression (VDSC) engines
// and of the stream splitter in front of them.
//
// Every pipe of display version 12 and later has two engines of its own
// (DSC0 and DSC1, for the left and the right half of a line split into
// two streams), with their control (PIPE_DSS_CTL1/2) next to them.
// Version 11 has those on pipes B and C only; its embedded-panel
// transcoder compresses with the transcoder-level engines DSCA and DSCC
// behind DSS_CTL1/2.  Each engine takes the picture parameter set (PPS)
// as registers 0..10 and 16 (17 and 18 from version 14 on), the rate
// control buffer thresholds and the fifteen rate control ranges.
//
// A pipe-level register sits at 0x200 per pipe from pipe B's address,
// so pipe A's is 0x200 below pipe B's.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2023 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_VDSC_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_VDSC_REGS_H

#include <kernel/dev/gpu/gpu_util.h>

/* pipe-level register: pipe B's address, 0x200 apart */
#define INTEL_VDSC_PIPE_REG(pb, pipe) ((uint32_t)((int)(pb) + ((int)(pipe) - 1) * 0x200))

/* ---- the stream splitter (DSS) -------------------------------------------- */

#define DSS_CTL1 0x67400
#define SPLITTER_ENABLE (1u << 31)
#define JOINER_ENABLE (1u << 30)
#define DUAL_LINK_MODE_INTERLEAVE (1u << 24)
#define DUAL_LINK_MODE_FRONTBACK (0u << 24)
#define OVERLAP_PIXELS_MASK (0xfu << 16)
#define OVERLAP_PIXELS(pixels) ((uint32_t)(pixels) << 16)
#define LEFT_DL_BUF_TARGET_DEPTH_MASK (0xfffu << 0)
#define LEFT_DL_BUF_TARGET_DEPTH(pixels) ((uint32_t)(pixels) << 0)
#define MAX_DL_BUFFER_TARGET_DEPTH 0x5a0

#define DSS_CTL2 0x67404
#define VDSC0_ENABLE (1u << 31)
#define VDSC2_ENABLE (1u << 30)
#define SMALL_JOINER_CONFIG_3_ENGINES (1u << 23)
#define VDSC1_ENABLE (1u << 15)
#define RIGHT_DL_BUF_TARGET_DEPTH_MASK (0xfffu << 0)
#define RIGHT_DL_BUF_TARGET_DEPTH(pixels) ((uint32_t)(pixels) << 0)

#define ICL_PIPE_DSS_CTL1(pipe) INTEL_VDSC_PIPE_REG(0x78200, pipe)
#define BIG_JOINER_ENABLE (1u << 29)
#define PRIMARY_BIG_JOINER_ENABLE (1u << 28)
#define VGA_CENTERING_ENABLE (1u << 27)
#define SPLITTER_CONFIGURATION_MASK GENMASK(26, 25)
#define ULTRA_JOINER_ENABLE (1u << 23)
#define PRIMARY_ULTRA_JOINER_ENABLE (1u << 22)
#define UNCOMPRESSED_JOINER_PRIMARY (1u << 21)
#define UNCOMPRESSED_JOINER_SECONDARY (1u << 20)

#define ICL_PIPE_DSS_CTL2(pipe) INTEL_VDSC_PIPE_REG(0x78204, pipe)

/* ---- picture parameter set ---------------------------------------------------- */

/* DSCA/DSCC: PPS 0..11 consecutive, 16.. after a gap of twelve */
#define DSCA_PPS(pps) (0x6B200u + (uint32_t)((pps) < 12 ? (pps) : (pps) + 12) * 4)
#define DSCC_PPS(pps) (0x6BA00u + (uint32_t)((pps) < 12 ? (pps) : (pps) + 12) * 4)
#define ICL_DSC0_PPS(pipe, pps) (INTEL_VDSC_PIPE_REG(0x78270, pipe) + (uint32_t)(pps) * 4)
#define ICL_DSC1_PPS(pipe, pps) (INTEL_VDSC_PIPE_REG(0x78370, pipe) + (uint32_t)(pps) * 4)

/* PPS 0 */
#define DSC_PPS0_NATIVE_422_ENABLE (1u << 23)
#define DSC_PPS0_NATIVE_420_ENABLE (1u << 22)
#define DSC_PPS0_ALT_ICH_SEL (1u << 20)
#define DSC_PPS0_VBR_ENABLE (1u << 19)
#define DSC_PPS0_422_ENABLE (1u << 18)
#define DSC_PPS0_COLOR_SPACE_CONVERSION (1u << 17)
#define DSC_PPS0_BLOCK_PREDICTION (1u << 16)
#define DSC_PPS0_LINE_BUF_DEPTH_MASK GENMASK(15, 12)
#define DSC_PPS0_LINE_BUF_DEPTH(depth) FIELD_PREP(DSC_PPS0_LINE_BUF_DEPTH_MASK, (uint32_t)(depth))
#define DSC_PPS0_BPC_MASK GENMASK(11, 8)
#define DSC_PPS0_BPC(bpc) FIELD_PREP(DSC_PPS0_BPC_MASK, (uint32_t)(bpc))
#define DSC_PPS0_VER_MINOR_MASK GENMASK(7, 4)
#define DSC_PPS0_VER_MINOR(minor) FIELD_PREP(DSC_PPS0_VER_MINOR_MASK, (uint32_t)(minor))
#define DSC_PPS0_VER_MAJOR_MASK GENMASK(3, 0)
#define DSC_PPS0_VER_MAJOR(major) FIELD_PREP(DSC_PPS0_VER_MAJOR_MASK, (uint32_t)(major))

/* PPS 1 */
#define DSC_PPS1_BPP_MASK GENMASK(9, 0)
#define DSC_PPS1_BPP(bpp) FIELD_PREP(DSC_PPS1_BPP_MASK, (uint32_t)(bpp))

/* PPS 2 */
#define DSC_PPS2_PIC_WIDTH_MASK GENMASK(31, 16)
#define DSC_PPS2_PIC_HEIGHT_MASK GENMASK(15, 0)
#define DSC_PPS2_PIC_WIDTH(w) FIELD_PREP(DSC_PPS2_PIC_WIDTH_MASK, (uint32_t)(w))
#define DSC_PPS2_PIC_HEIGHT(h) FIELD_PREP(DSC_PPS2_PIC_HEIGHT_MASK, (uint32_t)(h))

/* PPS 3 */
#define DSC_PPS3_SLICE_WIDTH_MASK GENMASK(31, 16)
#define DSC_PPS3_SLICE_HEIGHT_MASK GENMASK(15, 0)
#define DSC_PPS3_SLICE_WIDTH(w) FIELD_PREP(DSC_PPS3_SLICE_WIDTH_MASK, (uint32_t)(w))
#define DSC_PPS3_SLICE_HEIGHT(h) FIELD_PREP(DSC_PPS3_SLICE_HEIGHT_MASK, (uint32_t)(h))

/* PPS 4 */
#define DSC_PPS4_INITIAL_DEC_DELAY_MASK GENMASK(31, 16)
#define DSC_PPS4_INITIAL_XMIT_DELAY_MASK GENMASK(9, 0)
#define DSC_PPS4_INITIAL_DEC_DELAY(d) FIELD_PREP(DSC_PPS4_INITIAL_DEC_DELAY_MASK, (uint32_t)(d))
#define DSC_PPS4_INITIAL_XMIT_DELAY(d) FIELD_PREP(DSC_PPS4_INITIAL_XMIT_DELAY_MASK, (uint32_t)(d))

/* PPS 5 */
#define DSC_PPS5_SCALE_DEC_INT_MASK GENMASK(27, 16)
#define DSC_PPS5_SCALE_INC_INT_MASK GENMASK(15, 0)
#define DSC_PPS5_SCALE_DEC_INT(v) FIELD_PREP(DSC_PPS5_SCALE_DEC_INT_MASK, (uint32_t)(v))
#define DSC_PPS5_SCALE_INC_INT(v) FIELD_PREP(DSC_PPS5_SCALE_INC_INT_MASK, (uint32_t)(v))

/* PPS 6 */
#define DSC_PPS6_FLATNESS_MAX_QP_MASK GENMASK(28, 24)
#define DSC_PPS6_FLATNESS_MIN_QP_MASK GENMASK(20, 16)
#define DSC_PPS6_FIRST_LINE_BPG_OFFSET_MASK GENMASK(12, 8)
#define DSC_PPS6_INITIAL_SCALE_VALUE_MASK GENMASK(5, 0)
#define DSC_PPS6_FLATNESS_MAX_QP(qp) FIELD_PREP(DSC_PPS6_FLATNESS_MAX_QP_MASK, (uint32_t)(qp))
#define DSC_PPS6_FLATNESS_MIN_QP(qp) FIELD_PREP(DSC_PPS6_FLATNESS_MIN_QP_MASK, (uint32_t)(qp))
#define DSC_PPS6_FIRST_LINE_BPG_OFFSET(o) \
	FIELD_PREP(DSC_PPS6_FIRST_LINE_BPG_OFFSET_MASK, (uint32_t)(o))
#define DSC_PPS6_INITIAL_SCALE_VALUE(v) \
	FIELD_PREP(DSC_PPS6_INITIAL_SCALE_VALUE_MASK, (uint32_t)(v))

/* PPS 7 */
#define DSC_PPS7_NFL_BPG_OFFSET_MASK GENMASK(31, 16)
#define DSC_PPS7_SLICE_BPG_OFFSET_MASK GENMASK(15, 0)
#define DSC_PPS7_NFL_BPG_OFFSET(o) FIELD_PREP(DSC_PPS7_NFL_BPG_OFFSET_MASK, (uint32_t)(o))
#define DSC_PPS7_SLICE_BPG_OFFSET(o) FIELD_PREP(DSC_PPS7_SLICE_BPG_OFFSET_MASK, (uint32_t)(o))

/* PPS 8 */
#define DSC_PPS8_INITIAL_OFFSET_MASK GENMASK(31, 16)
#define DSC_PPS8_FINAL_OFFSET_MASK GENMASK(15, 0)
#define DSC_PPS8_INITIAL_OFFSET(o) FIELD_PREP(DSC_PPS8_INITIAL_OFFSET_MASK, (uint32_t)(o))
#define DSC_PPS8_FINAL_OFFSET(o) FIELD_PREP(DSC_PPS8_FINAL_OFFSET_MASK, (uint32_t)(o))

/* PPS 9 */
#define DSC_PPS9_RC_EDGE_FACTOR_MASK GENMASK(19, 16)
#define DSC_PPS9_RC_MODEL_SIZE_MASK GENMASK(15, 0)
#define DSC_PPS9_RC_EDGE_FACTOR(f) FIELD_PREP(DSC_PPS9_RC_EDGE_FACTOR_MASK, (uint32_t)(f))
#define DSC_PPS9_RC_MODEL_SIZE(s) FIELD_PREP(DSC_PPS9_RC_MODEL_SIZE_MASK, (uint32_t)(s))

/* PPS 10 */
#define DSC_PPS10_RC_TGT_OFF_LOW_MASK GENMASK(23, 20)
#define DSC_PPS10_RC_TGT_OFF_HIGH_MASK GENMASK(19, 16)
#define DSC_PPS10_RC_QUANT_INC_LIMIT1_MASK GENMASK(12, 8)
#define DSC_PPS10_RC_QUANT_INC_LIMIT0_MASK GENMASK(4, 0)
#define DSC_PPS10_RC_TARGET_OFF_LOW(v) FIELD_PREP(DSC_PPS10_RC_TGT_OFF_LOW_MASK, (uint32_t)(v))
#define DSC_PPS10_RC_TARGET_OFF_HIGH(v) FIELD_PREP(DSC_PPS10_RC_TGT_OFF_HIGH_MASK, (uint32_t)(v))
#define DSC_PPS10_RC_QUANT_INC_LIMIT1(l) \
	FIELD_PREP(DSC_PPS10_RC_QUANT_INC_LIMIT1_MASK, (uint32_t)(l))
#define DSC_PPS10_RC_QUANT_INC_LIMIT0(l) \
	FIELD_PREP(DSC_PPS10_RC_QUANT_INC_LIMIT0_MASK, (uint32_t)(l))

/* PPS 16 */
#define DSC_PPS16_SLICE_ROW_PR_FRME_MASK GENMASK(31, 20)
#define DSC_PPS16_SLICE_PER_LINE_MASK GENMASK(18, 16)
#define DSC_PPS16_SLICE_CHUNK_SIZE_MASK GENMASK(15, 0)
#define DSC_PPS16_SLICE_ROW_PER_FRAME(n) FIELD_PREP(DSC_PPS16_SLICE_ROW_PR_FRME_MASK, (uint32_t)(n))
#define DSC_PPS16_SLICE_PER_LINE(n) FIELD_PREP(DSC_PPS16_SLICE_PER_LINE_MASK, (uint32_t)(n))
#define DSC_PPS16_SLICE_CHUNK_SIZE(n) FIELD_PREP(DSC_PPS16_SLICE_CHUNK_SIZE_MASK, (uint32_t)(n))

/* PPS 17 (version 14 on) */
#define DSC_PPS17_SL_BPG_OFFSET_MASK GENMASK(31, 27)
#define DSC_PPS17_SL_BPG_OFFSET(o) FIELD_PREP(DSC_PPS17_SL_BPG_OFFSET_MASK, (uint32_t)(o))

/* PPS 18 (version 14 on) */
#define DSC_PPS18_NSL_BPG_OFFSET_MASK GENMASK(31, 16)
#define DSC_PPS18_SL_OFFSET_ADJ_MASK GENMASK(15, 0)
#define DSC_PPS18_NSL_BPG_OFFSET(o) FIELD_PREP(DSC_PPS18_NSL_BPG_OFFSET_MASK, (uint32_t)(o))
#define DSC_PPS18_SL_OFFSET_ADJ(o) FIELD_PREP(DSC_PPS18_SL_OFFSET_ADJ_MASK, (uint32_t)(o))

/* ---- rate control buffer thresholds (14 bytes in two register pairs) ---------- */

#define DSCA_RC_BUF_THRESH_0 0x6B230
#define DSCA_RC_BUF_THRESH_0_UDW (0x6B230 + 4)
#define DSCA_RC_BUF_THRESH_1 0x6B238
#define DSCA_RC_BUF_THRESH_1_UDW (0x6B238 + 4)
#define DSCC_RC_BUF_THRESH_0 0x6BA30
#define DSCC_RC_BUF_THRESH_0_UDW (0x6BA30 + 4)
#define DSCC_RC_BUF_THRESH_1 0x6BA38
#define DSCC_RC_BUF_THRESH_1_UDW (0x6BA38 + 4)
#define ICL_DSC0_RC_BUF_THRESH_0(pipe) INTEL_VDSC_PIPE_REG(0x78254, pipe)
#define ICL_DSC0_RC_BUF_THRESH_0_UDW(pipe) INTEL_VDSC_PIPE_REG(0x78254 + 4, pipe)
#define ICL_DSC0_RC_BUF_THRESH_1(pipe) INTEL_VDSC_PIPE_REG(0x7825C, pipe)
#define ICL_DSC0_RC_BUF_THRESH_1_UDW(pipe) INTEL_VDSC_PIPE_REG(0x7825C + 4, pipe)
#define ICL_DSC1_RC_BUF_THRESH_0(pipe) INTEL_VDSC_PIPE_REG(0x78354, pipe)
#define ICL_DSC1_RC_BUF_THRESH_0_UDW(pipe) INTEL_VDSC_PIPE_REG(0x78354 + 4, pipe)
#define ICL_DSC1_RC_BUF_THRESH_1(pipe) INTEL_VDSC_PIPE_REG(0x7835C, pipe)
#define ICL_DSC1_RC_BUF_THRESH_1_UDW(pipe) INTEL_VDSC_PIPE_REG(0x7835C + 4, pipe)

/* ---- rate control ranges (two 16-bit ranges a register, eight registers) ------- */

#define DSCA_RC_RANGE_PARAMETERS_0 0x6B240
#define DSCC_RC_RANGE_PARAMETERS_0 0x6BA40
#define ICL_DSC0_RC_RANGE_PARAMETERS_0(pipe) INTEL_VDSC_PIPE_REG(0x78208, pipe)
#define ICL_DSC1_RC_RANGE_PARAMETERS_0(pipe) INTEL_VDSC_PIPE_REG(0x78308, pipe)
/* register n (0..7: _0, _0_UDW, _1, _1_UDW, ... _3_UDW) of a set */
#define DSC_RC_RANGE_PARAMETERS_N(base, n) ((uint32_t)(base) + (uint32_t)(n) * 4)
#define RC_BPG_OFFSET_SHIFT 10
#define RC_MAX_QP_SHIFT 5
#define RC_MIN_QP_SHIFT 0

/* ---- what goes with compression elsewhere ------------------------------------ */

/* The DisplayPort transport: forward error correction on the link (the
 * transcoder's DP_TP_CTL from Tiger Lake on, the port's before). */
#ifndef DP_TP_CTL_FEC_ENABLE
#define DP_TP_CTL_FEC_ENABLE (1u << 30)
#endif
#ifndef DP_TP_STATUS_FEC_ENABLE_LIVE
#define DP_TP_STATUS_FEC_ENABLE_LIVE (1u << 28)
#endif

/* The picture parameter set as a secondary data packet on the link: the
 * transcoder's video DIP, 4 header and 128 payload bytes. */
#ifndef VDIP_ENABLE_PPS
#define VDIP_ENABLE_PPS (1u << 24)
#endif
#define VIDEO_DIP_PPS_DATA_SIZE 132
#define ICL_VIDEO_DIP_PPS_DATA(trans, i) (TRANS_BASE(trans) + 0x350 + (uint32_t)(i) * 4)

/* The register values for one engine of a picture parameter set:
 * pps[n] for PPS register n (0..10, 16..18; the others unused), the four
 * buffer threshold words and the eight range words. */
struct intel_vdsc_regs {
	uint32_t pps[19];
	uint32_t rc_buf_thresh[4];
	uint32_t rc_range[8];
};

struct drm_dsc_config;

/* The register values of parameter set `c' for one of
 * `num_vdsc_instances' engines (each takes 1/n of the line) on display
 * version `display_ver' (PPS 17/18 from 14 on); intel_vdsc.c. */
void intel_vdsc_regs_pack(const struct drm_dsc_config *c, int num_vdsc_instances, int display_ver,
			  struct intel_vdsc_regs *r);

#endif
