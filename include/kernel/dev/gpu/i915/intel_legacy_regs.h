// LikeOS -- display registers of the Intel parts before DDI.
//
// Generation 2 to 7 (i830 to Ivy Bridge) plus Valleyview and Cherryview:
// per-pipe DPLLs, pipes and their timing generators, the primary plane
// and cursor, the fixed-function ports (analog, LVDS, SDVO, DVO, HDMI,
// DisplayPort), the south display (PCH) of Ironlake to Ivy Bridge with its
// FDI links, transcoders and PLLs, the panel power sequencer, backlight
// PWM and panel fitter, the interrupt and hotplug registers.
//
// Every offset is the one the classic GMCH and PCH parts use.  Valleyview
// and Cherryview keep the same display registers but move the whole block
// up by 0x180000; the accessors in intel_legacy.h add that base, so the
// same definitions serve every platform.  Where a register exists only on
// one family, the comment says which.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2022-2026 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_INTEL_LEGACY_REGS_H
#define KERNEL_DEV_GPU_I915_INTEL_LEGACY_REGS_H

/* Valleyview and Cherryview: where the display block sits. */
#define VLV_DISPLAY_BASE 0x180000u

/* ---- per-pipe register spacing ------------------------------------------- */

/* Pipe C of Cherryview sits apart from A and B; everywhere else the pipes
 * are 0x1000 apart.  `d' is the struct lg_display of intel_legacy.h. */
#define LG_PIPE_OFF(d, p) (((p) == 2 && (d)->is_chv) ? 0x4000u : (uint32_t)(p) * 0x1000u)
#define LG_TRANS_OFF(d, p) (((p) == 2 && (d)->is_chv) ? 0x3000u : (uint32_t)(p) * 0x1000u)
/* Cursors: A at 0x70080, B at 0x700c0 on the GMCH parts and Ironlake/Sandy
 * Bridge, C at 0x700e0 on Cherryview; Ivy Bridge moved them to their pipe. */
#define LG_CURSOR_OFF(d, p)                                                    \
	((d)->is_ivb ? (uint32_t)(p) * 0x1000u :                               \
	 ((p) == 2 ? 0x60u : (uint32_t)(p) * 0x40u))

/* ---- clocks and DPLLs ------------------------------------------------------ */

#define DPLL(d, p) (((p) == 2 && (d)->is_chv) ? 0x6030u : 0x6014u + (uint32_t)(p) * 4u)
#define DPLL_MD(d, p) (((p) == 2 && (d)->is_chv) ? 0x603cu : 0x601cu + (uint32_t)(p) * 4u)
#define FP0(p) (0x6040u + (uint32_t)(p) * 8u)
#define FP1(p) (0x6044u + (uint32_t)(p) * 8u)

#define VGA0 0x6000u
#define VGA1 0x6004u
#define VGA_PD 0x6010u
#define VGA0_PD_P2_DIV_4 (1u << 7)
#define VGA0_PD_P1_DIV_2 (1u << 5)
#define VGA1_PD_P2_DIV_4 (1u << 15)
#define VGA1_PD_P1_DIV_2 (1u << 13)

#define DPLL_VCO_ENABLE (1u << 31)
#define DPLL_SDVO_HIGH_SPEED (1u << 30)
#define DPLL_DVO_2X_MODE (1u << 30)
#define DPLL_EXT_BUFFER_ENABLE_VLV (1u << 30)
#define DPLL_SYNCLOCK_ENABLE (1u << 29)
#define DPLL_REF_CLK_ENABLE_VLV (1u << 29)
#define DPLL_VGA_MODE_DIS (1u << 28)
#define DPLLB_MODE_DAC_SERIAL (1u << 26)
#define DPLLB_MODE_LVDS (2u << 26)
#define DPLL_MODE_MASK (3u << 26)
#define DPLL_DAC_SERIAL_P2_CLOCK_DIV_10 (0u << 24)
#define DPLL_DAC_SERIAL_P2_CLOCK_DIV_5 (1u << 24)
#define DPLLB_LVDS_P2_CLOCK_DIV_14 (0u << 24)
#define DPLLB_LVDS_P2_CLOCK_DIV_7 (1u << 24)
#define DPLL_P2_CLOCK_DIV_MASK 0x03000000u
#define DPLL_FPA01_P1_POST_DIV_MASK 0x00ff0000u
#define DPLL_FPA01_P1_POST_DIV_MASK_PINEVIEW 0x00ff8000u
#define DPLL_FPA01_P1_POST_DIV_MASK_I830 0x001f0000u
#define DPLL_FPA01_P1_POST_DIV_MASK_I830_LVDS 0x003f0000u
#define DPLL_FPA01_P1_POST_DIV_SHIFT 16
#define DPLL_FPA01_P1_POST_DIV_SHIFT_PINEVIEW 15
#define DPLL_LOCK_VLV (1u << 15)
#define DPLL_INTEGRATED_CRI_CLK_VLV (1u << 14)
#define DPLL_INTEGRATED_REF_CLK_VLV (1u << 13)
#define DPLL_SSC_REF_CLK_CHV (1u << 13)
#define DPLL_PORTC_READY_MASK (0xfu << 4)
#define DPLL_PORTB_READY_MASK (0xfu)
#define PLL_P2_DIVIDE_BY_4 (1u << 23)
#define PLL_P1_DIVIDE_BY_TWO (1u << 21)
#define PLL_REF_INPUT_DREFCLK (0u << 13)
#define PLL_REF_INPUT_TVCLKINA (1u << 13)
#define PLL_REF_INPUT_TVCLKINBC (2u << 13)
#define PLLB_REF_INPUT_SPREADSPECTRUMIN (3u << 13)
#define PLL_REF_INPUT_MASK (3u << 13)
#define PLL_LOAD_PULSE_PHASE_SHIFT 9
#define PLL_LOAD_PULSE_PHASE_MASK (0xfu << 9)
#define PLL_REF_SDVO_HDMI_MULTIPLIER_SHIFT 9
#define PLL_REF_SDVO_HDMI_MULTIPLIER_MASK (7u << 9)
#define DPLL_FPA1_P1_POST_DIV_SHIFT 0
#define DPLL_FPA1_P1_POST_DIV_MASK 0xffu
#define DISPLAY_RATE_SELECT_FPA1 (1u << 8)
#define SDVO_MULTIPLIER_MASK 0x000000ffu
#define SDVO_MULTIPLIER_SHIFT_HIRES 4
#define SDVO_MULTIPLIER_SHIFT_VGA 0

#define DPLL_MD_UDI_DIVIDER_MASK 0x3f000000u
#define DPLL_MD_UDI_DIVIDER_SHIFT 24
#define DPLL_MD_VGA_UDI_DIVIDER_MASK 0x003f0000u
#define DPLL_MD_VGA_UDI_DIVIDER_SHIFT 16
#define DPLL_MD_UDI_MULTIPLIER_MASK 0x00003f00u
#define DPLL_MD_UDI_MULTIPLIER_SHIFT 8
#define DPLL_MD_VGA_UDI_MULTIPLIER_MASK 0x0000003fu
#define DPLL_MD_VGA_UDI_MULTIPLIER_SHIFT 0

#define FP_N_DIV_MASK 0x003f0000u
#define FP_N_PINEVIEW_DIV_MASK 0x00ff0000u
#define FP_N_DIV_SHIFT 16
#define FP_M1_DIV_MASK 0x00003f00u
#define FP_M1_DIV_SHIFT 8
#define FP_M2_DIV_MASK 0x0000003fu
#define FP_M2_PINEVIEW_DIV_MASK 0x000000ffu
#define FP_M2_DIV_SHIFT 0
#define FP_CB_TUNE (0x3u << 22)

/* Valleyview / Cherryview clocks (display-relative) */
#define RAWCLK_FREQ_VLV 0x6024u
#define FW_BLC_SELF_VLV 0x6500u
#define FW_CSPWRDWNEN (1u << 15)
#define MI_ARB_VLV 0x6504u
#define MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE_VLV (1u << 2)
#define CZCLK_CDCLK_FREQ_RATIO 0x6508u
#define CDCLK_FREQ_SHIFT 4
#define CDCLK_FREQ_MASK (0x1fu << 4)
#define CZCLK_FREQ_MASK 0xfu
#define GCI_CONTROL 0x650cu
#define PFI_CREDIT_63 (9u << 28)
#define PFI_CREDIT_31 (8u << 28)
#define PFI_CREDIT(x) (((uint32_t)(x) - 8u) << 28)
#define PFI_CREDIT_RESEND (1u << 27)
#define VGA_FAST_MODE_DISABLE (1u << 14)
#define GMBUSFREQ_VLV 0x6510u
#define CBR1_VLV 0x70400u
#define CBR_PND_DEADLINE_DISABLE (1u << 31)
#define CBR_PWM_CLOCK_MUX_SELECT (1u << 30)
#define CBR4_VLV 0x70450u
#define CBR_DPLLBMD_PIPE(p) (1u << (7 + (p) * 11))

#define DSPCLK_GATE_D 0x6200u
#define DPUNIT_B_CLOCK_GATE_DISABLE (1u << 30)
#define VSUNIT_CLOCK_GATE_DISABLE (1u << 29)
#define VRHUNIT_CLOCK_GATE_DISABLE (1u << 28)
#define VRDUNIT_CLOCK_GATE_DISABLE (1u << 27)
#define AUDUNIT_CLOCK_GATE_DISABLE (1u << 26)
#define DPUNIT_A_CLOCK_GATE_DISABLE (1u << 25)
#define DPCUNIT_CLOCK_GATE_DISABLE (1u << 24)
#define PNV_GMBUSUNIT_CLOCK_GATE_DISABLE (1u << 24)
#define TVRUNIT_CLOCK_GATE_DISABLE (1u << 23)
#define TVCUNIT_CLOCK_GATE_DISABLE (1u << 22)
#define TVFUNIT_CLOCK_GATE_DISABLE (1u << 21)
#define TVEUNIT_CLOCK_GATE_DISABLE (1u << 20)
#define DVSUNIT_CLOCK_GATE_DISABLE (1u << 19)
#define DSSUNIT_CLOCK_GATE_DISABLE (1u << 18)
#define DDBUNIT_CLOCK_GATE_DISABLE (1u << 17)
#define DPRUNIT_CLOCK_GATE_DISABLE (1u << 16)
#define DPFUNIT_CLOCK_GATE_DISABLE (1u << 15)
#define DPBMUNIT_CLOCK_GATE_DISABLE (1u << 14)
#define DPLSUNIT_CLOCK_GATE_DISABLE (1u << 13)
#define DPLUNIT_CLOCK_GATE_DISABLE (1u << 12)
#define DPOUNIT_CLOCK_GATE_DISABLE (1u << 11)
#define DPBUNIT_CLOCK_GATE_DISABLE (1u << 10)
#define DCUNIT_CLOCK_GATE_DISABLE (1u << 9)
#define DPUNIT_CLOCK_GATE_DISABLE (1u << 8)
#define VRUNIT_CLOCK_GATE_DISABLE (1u << 7)
#define OVHUNIT_CLOCK_GATE_DISABLE (1u << 6)
#define DPIOUNIT_CLOCK_GATE_DISABLE (1u << 6)
#define OVFUNIT_CLOCK_GATE_DISABLE (1u << 5)
#define OVBUNIT_CLOCK_GATE_DISABLE (1u << 4)
#define OVRUNIT_CLOCK_GATE_DISABLE (1u << 3)
#define OVCUNIT_CLOCK_GATE_DISABLE (1u << 2)
#define OVUUNIT_CLOCK_GATE_DISABLE (1u << 1)
#define ZVUNIT_CLOCK_GATE_DISABLE (1u << 0)

/* ---- pipe timings (transcoder offsets) -------------------------------------- */

#define TRANS_HTOTAL(d, t) (0x60000u + LG_TRANS_OFF(d, t))
#define TRANS_HBLANK(d, t) (0x60004u + LG_TRANS_OFF(d, t))
#define TRANS_HSYNC(d, t) (0x60008u + LG_TRANS_OFF(d, t))
#define TRANS_VTOTAL(d, t) (0x6000cu + LG_TRANS_OFF(d, t))
#define TRANS_VBLANK(d, t) (0x60010u + LG_TRANS_OFF(d, t))
#define TRANS_VSYNC(d, t) (0x60014u + LG_TRANS_OFF(d, t))
#define PIPESRC(d, p) (0x6001cu + LG_TRANS_OFF(d, p))
#define BCLRPAT(d, t) (0x60020u + LG_TRANS_OFF(d, t))
#define TRANS_VSYNCSHIFT(d, t) (0x60028u + LG_TRANS_OFF(d, t))
#define TRANS_MULT(d, t) (0x6002cu + LG_TRANS_OFF(d, t))

/* M/N of the CPU transcoder: Ironlake on and Cherryview. */
#define PIPE_DATA_M1(d, t) (0x60030u + LG_TRANS_OFF(d, t))
#define PIPE_DATA_N1(d, t) (0x60034u + LG_TRANS_OFF(d, t))
#define PIPE_DATA_M2(d, t) (0x60038u + LG_TRANS_OFF(d, t))
#define PIPE_DATA_N2(d, t) (0x6003cu + LG_TRANS_OFF(d, t))
#define PIPE_LINK_M1(d, t) (0x60040u + LG_TRANS_OFF(d, t))
#define PIPE_LINK_N1(d, t) (0x60044u + LG_TRANS_OFF(d, t))
#define PIPE_LINK_M2(d, t) (0x60048u + LG_TRANS_OFF(d, t))
#define PIPE_LINK_N2(d, t) (0x6004cu + LG_TRANS_OFF(d, t))
/* M/N of G4X and Valleyview (the pipe's own registers). */
#define PIPE_DATA_M_G4X(p) (0x70050u + (uint32_t)(p) * 0x1000u)
#define PIPE_DATA_N_G4X(p) (0x70054u + (uint32_t)(p) * 0x1000u)
#define PIPE_LINK_M_G4X(p) (0x70060u + (uint32_t)(p) * 0x1000u)
#define PIPE_LINK_N_G4X(p) (0x70064u + (uint32_t)(p) * 0x1000u)
#define TU_SIZE(x) (((uint32_t)(x) - 1u) << 25)
#define TU_SIZE_MASK (0x3fu << 25)
#define DATA_LINK_M_N_MASK 0xffffffu
#define DATA_LINK_N_MAX 0x800000u

/* CHV pipe B blender */
#define CHV_BLEND(d, p) (0x60a00u + LG_TRANS_OFF(d, p))
#define CHV_BLEND_LEGACY (0u << 30)
#define CHV_CANVAS(d, p) (0x60a04u + LG_TRANS_OFF(d, p))
#define PRIMPOS(d, p) (0x60a08u + LG_TRANS_OFF(d, p))
#define PRIMSIZE(d, p) (0x60a0cu + LG_TRANS_OFF(d, p))
#define PRIMCNSTALPHA(d, p) (0x60a10u + LG_TRANS_OFF(d, p))

/* ---- pipes -------------------------------------------------------------------- */

#define PIPEDSL(d, p) (0x70000u + LG_PIPE_OFF(d, p))
#define PIPEDSL_LINE_MASK 0xfffffu
#define PIPEDSL_LINE_MASK_GEN2 0xfffu
#define PIPECONF(d, p) (0x70008u + LG_PIPE_OFF(d, p))
#define PIPECONF_ENABLE (1u << 31)
#define PIPECONF_DOUBLE_WIDE (1u << 30) /* pre-i965 */
#define PIPECONF_STATE_ENABLE (1u << 30) /* i965+ */
#define PIPECONF_DSI_PLL_LOCKED (1u << 29) /* vlv, pipe A */
#define PIPECONF_FRAME_START_DELAY_MASK (3u << 27)
#define PIPECONF_FRAME_START_DELAY(x) ((uint32_t)(x) << 27)
#define PIPECONF_PIPE_LOCKED (1u << 25)
#define PIPECONF_FORCE_BORDER (1u << 25)
#define PIPECONF_GAMMA_MODE_MASK_I9XX (1u << 24)
#define PIPECONF_GAMMA_MODE_MASK_ILK (3u << 24)
#define PIPECONF_GAMMA_MODE_8BIT (0u << 24)
#define PIPECONF_GAMMA_MODE_10BIT (1u << 24)
#define PIPECONF_INTERLACE_MASK (7u << 21)
#define PIPECONF_INTERLACE_PROGRESSIVE (0u << 21)
#define PIPECONF_INTERLACE_W_SYNC_SHIFT_PANEL (4u << 21)
#define PIPECONF_INTERLACE_W_SYNC_SHIFT (5u << 21)
#define PIPECONF_INTERLACE_W_FIELD_INDICATION (6u << 21)
#define PIPECONF_INTERLACE_FIELD_0_ONLY (7u << 21)
#define PIPECONF_INTERLACE_PF_PD_ILK (0u << 21)
#define PIPECONF_INTERLACE_PF_ID_ILK (1u << 21)
#define PIPECONF_INTERLACE_IF_ID_ILK (3u << 21)
#define PIPECONF_REFRESH_RATE_ALT_ILK (1u << 20)
#define PIPECONF_MSA_TIMING_DELAY(x) ((uint32_t)(x) << 18)
#define PIPECONF_CXSR_DOWNCLOCK (1u << 16)
#define PIPECONF_WGC_ENABLE (1u << 15)
#define PIPECONF_COLOR_RANGE_SELECT (1u << 13)
#define PIPECONF_OUTPUT_COLORSPACE_MASK (3u << 11)
#define PIPECONF_BPC_MASK (7u << 5)
#define PIPECONF_BPC_8 (0u << 5)
#define PIPECONF_BPC_10 (1u << 5)
#define PIPECONF_BPC_6 (2u << 5)
#define PIPECONF_BPC_12 (3u << 5)
#define PIPECONF_DITHER_EN (1u << 4)
#define PIPECONF_DITHER_TYPE_MASK (3u << 2)
#define PIPECONF_DITHER_TYPE_SP (0u << 2)

#define PIPESTAT(d, p) (0x70024u + LG_PIPE_OFF(d, p))
#define PIPE_FIFO_UNDERRUN_STATUS (1u << 31)
#define SPRITE1_FLIP_DONE_INT_EN_VLV (1u << 30)
#define PIPE_CRC_ERROR_ENABLE (1u << 29)
#define PIPE_CRC_DONE_ENABLE (1u << 28)
#define PIPE_GMBUS_EVENT_ENABLE (1u << 27)
#define PLANE_FLIP_DONE_INT_EN_VLV (1u << 26)
#define PIPE_HOTPLUG_INTERRUPT_ENABLE (1u << 26)
#define PIPE_VSYNC_INTERRUPT_ENABLE (1u << 25)
#define PIPE_DISPLAY_LINE_COMPARE_ENABLE (1u << 24)
#define PIPE_DPST_EVENT_ENABLE (1u << 23)
#define SPRITE0_FLIP_DONE_INT_EN_VLV (1u << 22)
#define PIPE_LEGACY_BLC_EVENT_ENABLE (1u << 22)
#define PIPE_ODD_FIELD_INTERRUPT_ENABLE (1u << 21)
#define PIPE_EVEN_FIELD_INTERRUPT_ENABLE (1u << 20)
#define PIPE_HOTPLUG_TV_INTERRUPT_ENABLE (1u << 18) /* pre-965 */
#define PIPE_START_VBLANK_INTERRUPT_ENABLE (1u << 18) /* 965+ */
#define PIPE_FRAMESTART_INTERRUPT_ENABLE (1u << 17)
#define PIPE_VBLANK_INTERRUPT_ENABLE (1u << 17)
#define PIPE_OVERLAY_UPDATED_ENABLE (1u << 16)
#define SPRITE1_FLIP_DONE_INT_STATUS_VLV (1u << 15)
#define SPRITE0_FLIP_DONE_INT_STATUS_VLV (1u << 14)
#define PIPE_CRC_ERROR_INTERRUPT_STATUS (1u << 13)
#define PIPE_CRC_DONE_INTERRUPT_STATUS (1u << 12)
#define PIPE_GMBUS_INTERRUPT_STATUS (1u << 11)
#define PLANE_FLIP_DONE_INT_STATUS_VLV (1u << 10)
#define PIPE_HOTPLUG_INTERRUPT_STATUS (1u << 10)
#define PIPE_VSYNC_INTERRUPT_STATUS (1u << 9)
#define PIPE_DISPLAY_LINE_COMPARE_STATUS (1u << 8)
#define PIPE_DPST_EVENT_STATUS (1u << 7)
#define PIPE_LEGACY_BLC_EVENT_STATUS (1u << 6)
#define PIPE_ODD_FIELD_INTERRUPT_STATUS (1u << 5)
#define PIPE_EVEN_FIELD_INTERRUPT_STATUS (1u << 4)
#define PIPE_HOTPLUG_TV_INTERRUPT_STATUS (1u << 2) /* pre-965 */
#define PIPE_START_VBLANK_INTERRUPT_STATUS (1u << 2) /* 965+ */
#define PIPE_FRAMESTART_INTERRUPT_STATUS (1u << 1)
#define PIPE_VBLANK_INTERRUPT_STATUS (1u << 1)
#define PIPE_OVERLAY_UPDATED_STATUS (1u << 0)
#define PIPESTAT_INT_ENABLE_MASK 0x7fff0000u
#define PIPESTAT_INT_STATUS_MASK 0x0000ffffu

/* Frame counter: pre-G4X a 16+8 bit split counter, G4X on a register. */
#define PIPEFRAME(d, p) (0x70040u + LG_PIPE_OFF(d, p))
#define PIPE_FRAME_HIGH_MASK 0x0000ffffu
#define PIPEFRAMEPIXEL(d, p) (0x70044u + LG_PIPE_OFF(d, p))
#define PIPE_FRAME_LOW_MASK 0xff000000u
#define PIPE_FRAME_LOW_SHIFT 24
#define PIPE_PIXEL_MASK 0x00ffffffu
#define PIPE_FRMCOUNT_G4X(d, p) (0x70040u + LG_PIPE_OFF(d, p))
#define VLV_PIPE_MSA_MISC(d, p) (0x70048u + LG_PIPE_OFF(d, p))
#define PIPEGCMAX(d, p, i) (0x70010u + LG_PIPE_OFF(d, p) + (uint32_t)(i) * 4u)

/* ---- the primary plane (plane offsets follow the pipes) ---------------------- */

#define DSPADDR_VLV(d, pl) (0x7017cu + LG_PIPE_OFF(d, pl))
#define DSPCNTR(d, pl) (0x70180u + LG_PIPE_OFF(d, pl))
#define DISP_ENABLE (1u << 31)
#define DISP_PIPE_GAMMA_ENABLE (1u << 30)
#define DISP_FORMAT_MASK (0xfu << 26)
#define DISP_FORMAT_8BPP (2u << 26)
#define DISP_FORMAT_BGRA555 (3u << 26)
#define DISP_FORMAT_BGRX555 (4u << 26)
#define DISP_FORMAT_BGRX565 (5u << 26)
#define DISP_FORMAT_BGRX888 (6u << 26)
#define DISP_FORMAT_BGRA888 (7u << 26)
#define DISP_FORMAT_RGBX101010 (8u << 26)
#define DISP_FORMAT_RGBA101010 (9u << 26)
#define DISP_FORMAT_BGRX101010 (10u << 26)
#define DISP_FORMAT_BGRA101010 (11u << 26)
#define DISP_FORMAT_RGBX161616 (12u << 26)
#define DISP_FORMAT_RGBX888 (14u << 26)
#define DISP_FORMAT_RGBA888 (15u << 26)
#define DISP_STEREO_ENABLE (1u << 25)
#define DISP_PIPE_CSC_ENABLE (1u << 24) /* ilk+ */
#define DISP_PIPE_SEL_MASK (3u << 24)
#define DISP_PIPE_SEL(p) ((uint32_t)(p) << 24) /* pre-i965 */
#define DISP_SRC_KEY_ENABLE (1u << 22)
#define DISP_LINE_DOUBLE (1u << 20)
#define DISP_ALPHA_PREMULTIPLY (1u << 16)
#define DISP_ROTATE_180 (1u << 15)
#define DISP_ALPHA_TRANS_ENABLE (1u << 15)
#define DISP_TRICKLE_FEED_DISABLE (1u << 14) /* g4x+ */
#define DISP_TILED (1u << 10) /* i965+ */
#define DISP_ASYNC_FLIP (1u << 9)
#define DISP_MIRROR (1u << 8)
#define DISP_SPRITE_ABOVE_OVERLAY (1u << 0)
#define DSPADDR(d, pl) (0x70184u + LG_PIPE_OFF(d, pl)) /* pre-i965 */
#define DSPLINOFF(d, pl) (0x70184u + LG_PIPE_OFF(d, pl)) /* i965+ */
#define DSPSTRIDE(d, pl) (0x70188u + LG_PIPE_OFF(d, pl))
#define DSPPOS(d, pl) (0x7018cu + LG_PIPE_OFF(d, pl)) /* pre-g4x */
#define DSPSIZE(d, pl) (0x70190u + LG_PIPE_OFF(d, pl)) /* pre-g4x */
#define DSPSURF(d, pl) (0x7019cu + LG_PIPE_OFF(d, pl)) /* i965+ */
#define DSPTILEOFF(d, pl) (0x701a4u + LG_PIPE_OFF(d, pl)) /* i965+ */
#define DSPSURFLIVE(d, pl) (0x701acu + LG_PIPE_OFF(d, pl)) /* g4x+ */

/* ---- the cursor ---------------------------------------------------------------- */

#define CURCNTR(d, p) (0x70080u + LG_CURSOR_OFF(d, p))
/* i845/i865 */
#define CURSOR_ENABLE (1u << 31)
#define CURSOR_PIPE_GAMMA_ENABLE (1u << 30)
#define CURSOR_STRIDE_SHIFT 28
#define CURSOR_STRIDE_MASK (3u << 28)
#define CURSOR_FORMAT_MASK (7u << 24)
#define CURSOR_FORMAT_ARGB (4u << 24)
#define CURSOR_FORMAT_XRGB (5u << 24)
/* i9xx and later */
#define MCURSOR_PIPE_SEL_MASK (3u << 28)
#define MCURSOR_PIPE_SEL(p) ((uint32_t)(p) << 28)
#define MCURSOR_PIPE_GAMMA_ENABLE (1u << 26)
#define MCURSOR_PIPE_CSC_ENABLE (1u << 24)
#define MCURSOR_ROTATE_180 (1u << 15)
#define MCURSOR_TRICKLE_FEED_DISABLE (1u << 14)
#define MCURSOR_MODE_MASK 0x27u
#define MCURSOR_MODE_DISABLE 0x00u
#define MCURSOR_MODE_128_32B_AX 0x02u
#define MCURSOR_MODE_256_32B_AX 0x03u
#define MCURSOR_MODE_64_32B_AX 0x07u
#define MCURSOR_MODE_128_ARGB_AX (0x20u | MCURSOR_MODE_128_32B_AX)
#define MCURSOR_MODE_256_ARGB_AX (0x20u | MCURSOR_MODE_256_32B_AX)
#define MCURSOR_MODE_64_ARGB_AX (0x20u | MCURSOR_MODE_64_32B_AX)
#define CURBASE(d, p) (0x70084u + LG_CURSOR_OFF(d, p))
#define CURPOS(d, p) (0x70088u + LG_CURSOR_OFF(d, p))
#define CURSOR_POS_Y_SIGN (1u << 31)
#define CURSOR_POS_X_SIGN (1u << 15)
#define CURSIZE(d, p) (0x700a0u + LG_CURSOR_OFF(d, p)) /* 845/865 */
#define CURSOR_HEIGHT(h) ((uint32_t)(h) << 12)
#define CURSOR_WIDTH(w) ((uint32_t)(w))
#define CURSURFLIVE(d, p) (0x700acu + LG_CURSOR_OFF(d, p))

/* ---- palettes -------------------------------------------------------------- */

/* GMCH (and Valleyview/Cherryview, display-relative): 256 x 8:8:8 */
#define PALETTE(d, p, i)                                                       \
	(((p) == 2 ? 0xc000u : 0xa000u + (uint32_t)(p) * 0x800u) + (uint32_t)(i) * 4u)
/* Ironlake on */
#define LGC_PALETTE(p, i) (0x4a000u + (uint32_t)(p) * 0x800u + (uint32_t)(i) * 4u)

/* ---- the legacy VGA plane ------------------------------------------------------- */

#define VGACNTRL 0x71400u /* GMCH and Valleyview/Cherryview (display-relative) */
#define CPU_VGACNTRL 0x41000u /* Ironlake on */
#define VGA_DISP_DISABLE (1u << 31)
#define VGA_2X_MODE (1u << 30)

/* ---- interrupts ---------------------------------------------------------------- */

/* GMCH (16 bits wide on gen2) and, display-relative, Valleyview/Cherryview */
#define GEN2_IER 0x20a0u
#define GEN2_IIR 0x20a4u
#define GEN2_IMR 0x20a8u
#define GEN2_ISR 0x20acu
#define I915_LPE_PIPE_B_INTERRUPT (1u << 21)
#define I915_LPE_PIPE_A_INTERRUPT (1u << 20)
#define I915_MIPIC_INTERRUPT (1u << 19)
#define I915_MIPIA_INTERRUPT (1u << 18)
#define I915_DISPLAY_PORT_INTERRUPT (1u << 17)
#define I915_DISPLAY_PIPE_C_HBLANK_INTERRUPT (1u << 16)
#define I915_MASTER_ERROR_INTERRUPT (1u << 15)
#define I915_DISPLAY_PIPE_B_HBLANK_INTERRUPT (1u << 14)
#define I915_DISPLAY_PIPE_A_HBLANK_INTERRUPT (1u << 13)
#define I915_LPE_PIPE_C_INTERRUPT (1u << 12)
#define I915_DISPLAY_PLANE_A_FLIP_PENDING_INTERRUPT (1u << 11)
#define I915_DISPLAY_PIPE_C_VBLANK_INTERRUPT (1u << 10)
#define I915_DISPLAY_PLANE_B_FLIP_PENDING_INTERRUPT (1u << 10)
#define I915_DISPLAY_PIPE_C_EVENT_INTERRUPT (1u << 9)
#define I915_DISPLAY_PIPE_A_VBLANK_INTERRUPT (1u << 7)
#define I915_DISPLAY_PIPE_A_EVENT_INTERRUPT (1u << 6)
#define I915_DISPLAY_PIPE_B_VBLANK_INTERRUPT (1u << 5)
#define I915_DISPLAY_PIPE_B_EVENT_INTERRUPT (1u << 4)
#define I915_ASLE_INTERRUPT (1u << 0)

/* Ironlake to Ivy Bridge: the north display */
#define DEISR 0x44000u
#define DEIMR 0x44004u
#define DEIIR 0x44008u
#define DEIER 0x4400cu
#define DE_MASTER_IRQ_CONTROL (1u << 31)
#define DE_SPRITEB_FLIP_DONE (1u << 29)
#define DE_SPRITEA_FLIP_DONE (1u << 28)
#define DE_PLANEB_FLIP_DONE (1u << 27)
#define DE_PLANEA_FLIP_DONE (1u << 26)
#define DE_PLANE_FLIP_DONE(pl) (1u << (26 + (pl)))
#define DE_PCU_EVENT (1u << 25)
#define DE_GTT_FAULT (1u << 24)
#define DE_POISON (1u << 23)
#define DE_PERFORM_COUNTER (1u << 22)
#define DE_PCH_EVENT (1u << 21)
#define DE_AUX_CHANNEL_A (1u << 20)
#define DE_DP_A_HOTPLUG (1u << 19)
#define DE_GSE (1u << 18)
#define DE_PIPEB_VBLANK (1u << 15)
#define DE_PIPEB_FIFO_UNDERRUN (1u << 8)
#define DE_PIPEA_VBLANK (1u << 7)
#define DE_PIPE_VBLANK(p) (1u << (7 + 8 * (p)))
#define DE_PIPEA_FIFO_UNDERRUN (1u << 0)
#define DE_PIPE_FIFO_UNDERRUN(p) (1u << (8 * (p)))
/* Ivy Bridge moved them */
#define DE_ERR_INT_IVB (1u << 30)
#define DE_GSE_IVB (1u << 29)
#define DE_PCH_EVENT_IVB (1u << 28)
#define DE_DP_A_HOTPLUG_IVB (1u << 27)
#define DE_AUX_CHANNEL_A_IVB (1u << 26)
#define DE_PLANE_FLIP_DONE_IVB(pl) (1u << (3 + 5 * (pl)))
#define DE_PIPE_VBLANK_IVB(p) (1u << ((p) * 5))
#define GEN7_ERR_INT 0x44040u
#define ERR_INT_POISON (1u << 31)
#define ERR_INT_FIFO_UNDERRUN(p) (1u << ((p) * 3))
#define DIGITAL_PORT_HOTPLUG_CNTRL 0x44030u
#define DIGITAL_PORTA_HOTPLUG_ENABLE (1u << 4)
#define DIGITAL_PORTA_PULSE_DURATION_2ms (0u << 2)
#define DIGITAL_PORTA_PULSE_DURATION_MASK (3u << 2)
#define DIGITAL_PORTA_HOTPLUG_STATUS_MASK (3u << 0)
#define DIGITAL_PORTA_HOTPLUG_SHORT_DETECT (1u << 0)
#define DIGITAL_PORTA_HOTPLUG_LONG_DETECT (2u << 0)

/* The south display (PCH) */
#define SDEISR 0xc4000u
#define SDEIMR 0xc4004u
#define SDEIIR 0xc4008u
#define SDEIER 0xc400cu
/* Ibex Peak */
#define SDE_GMBUS (1u << 24)
#define SDE_FDI_RXB (1u << 17)
#define SDE_FDI_RXA (1u << 16)
#define SDE_AUXD (1u << 15)
#define SDE_AUXC (1u << 14)
#define SDE_AUXB (1u << 13)
#define SDE_AUX_MASK (7u << 13)
#define SDE_CRT_HOTPLUG (1u << 11)
#define SDE_PORTD_HOTPLUG (1u << 10)
#define SDE_PORTC_HOTPLUG (1u << 9)
#define SDE_PORTB_HOTPLUG (1u << 8)
#define SDE_SDVOB_HOTPLUG (1u << 6)
#define SDE_HOTPLUG_MASK (SDE_CRT_HOTPLUG | SDE_SDVOB_HOTPLUG | SDE_PORTB_HOTPLUG | \
			  SDE_PORTC_HOTPLUG | SDE_PORTD_HOTPLUG)
#define SDE_TRANSB_FIFO_UNDER (1u << 3)
#define SDE_TRANSA_FIFO_UNDER (1u << 0)
/* Cougar / Panther Point */
#define SDE_AUXD_CPT (1u << 27)
#define SDE_AUXC_CPT (1u << 26)
#define SDE_AUXB_CPT (1u << 25)
#define SDE_AUX_MASK_CPT (7u << 25)
#define SDE_PORTD_HOTPLUG_CPT (1u << 23)
#define SDE_PORTC_HOTPLUG_CPT (1u << 22)
#define SDE_PORTB_HOTPLUG_CPT (1u << 21)
#define SDE_CRT_HOTPLUG_CPT (1u << 19)
#define SDE_SDVOB_HOTPLUG_CPT (1u << 18)
#define SDE_HOTPLUG_MASK_CPT (SDE_CRT_HOTPLUG_CPT | SDE_SDVOB_HOTPLUG_CPT |         \
			      SDE_PORTD_HOTPLUG_CPT | SDE_PORTC_HOTPLUG_CPT |   \
			      SDE_PORTB_HOTPLUG_CPT)
#define SDE_GMBUS_CPT (1u << 17)
#define SDE_ERROR_CPT (1u << 16)
#define SERR_INT 0xc4040u
#define SERR_INT_POISON (1u << 31)
#define SERR_INT_TRANS_FIFO_UNDERRUN(p) (1u << ((p) * 3))

/* ---- hotplug ------------------------------------------------------------------- */

/* GMCH (gen3+) and Valleyview/Cherryview (display-relative) */
#define PORT_HOTPLUG_EN 0x61110u
#define PORTB_HOTPLUG_INT_EN (1u << 29)
#define PORTC_HOTPLUG_INT_EN (1u << 28)
#define PORTD_HOTPLUG_INT_EN (1u << 27)
#define SDVOB_HOTPLUG_INT_EN (1u << 26)
#define SDVOC_HOTPLUG_INT_EN (1u << 25)
#define TV_HOTPLUG_INT_EN (1u << 18)
#define CRT_HOTPLUG_INT_EN (1u << 9)
#define HOTPLUG_INT_EN_MASK (PORTB_HOTPLUG_INT_EN | PORTC_HOTPLUG_INT_EN |            \
			     PORTD_HOTPLUG_INT_EN | SDVOC_HOTPLUG_INT_EN |            \
			     SDVOB_HOTPLUG_INT_EN | CRT_HOTPLUG_INT_EN)
#define CRT_HOTPLUG_FORCE_DETECT (1u << 3)
#define CRT_HOTPLUG_ACTIVATION_PERIOD_32 (0u << 8)
#define CRT_HOTPLUG_ACTIVATION_PERIOD_64 (1u << 8)
#define CRT_HOTPLUG_DAC_ON_TIME_2M (0u << 7)
#define CRT_HOTPLUG_DAC_ON_TIME_4M (1u << 7)
#define CRT_HOTPLUG_VOLTAGE_COMPARE_40 (0u << 5)
#define CRT_HOTPLUG_VOLTAGE_COMPARE_50 (1u << 5)
#define CRT_HOTPLUG_VOLTAGE_COMPARE_60 (2u << 5)
#define CRT_HOTPLUG_VOLTAGE_COMPARE_70 (3u << 5)
#define CRT_HOTPLUG_VOLTAGE_COMPARE_MASK (3u << 5)
#define CRT_HOTPLUG_DETECT_DELAY_1G (0u << 4)
#define CRT_HOTPLUG_DETECT_DELAY_2G (1u << 4)
#define CRT_HOTPLUG_DETECT_VOLTAGE_325MV (0u << 2)
#define CRT_HOTPLUG_DETECT_VOLTAGE_475MV (1u << 2)
#define PORT_HOTPLUG_STAT 0x61114u
#define PORTD_HOTPLUG_LIVE_STATUS_G4X (1u << 27)
#define PORTC_HOTPLUG_LIVE_STATUS_G4X (1u << 28)
#define PORTB_HOTPLUG_LIVE_STATUS_G4X (1u << 29)
#define PORTD_HOTPLUG_INT_STATUS (3u << 21)
#define PORTD_HOTPLUG_INT_LONG_PULSE (2u << 21)
#define PORTD_HOTPLUG_INT_SHORT_PULSE (1u << 21)
#define PORTC_HOTPLUG_INT_STATUS (3u << 19)
#define PORTC_HOTPLUG_INT_LONG_PULSE (2u << 19)
#define PORTC_HOTPLUG_INT_SHORT_PULSE (1u << 19)
#define PORTB_HOTPLUG_INT_STATUS (3u << 17)
#define PORTB_HOTPLUG_INT_LONG_PULSE (2u << 17)
#define PORTB_HOTPLUG_INT_SHORT_PULSE (1u << 17)
#define CRT_HOTPLUG_INT_STATUS (1u << 11)
#define TV_HOTPLUG_INT_STATUS (1u << 10)
#define CRT_HOTPLUG_MONITOR_MASK (3u << 8)
#define CRT_HOTPLUG_MONITOR_COLOR (3u << 8)
#define CRT_HOTPLUG_MONITOR_MONO (2u << 8)
#define CRT_HOTPLUG_MONITOR_NONE (0u << 8)
#define DP_AUX_CHANNEL_D_INT_STATUS_G4X (1u << 6)
#define DP_AUX_CHANNEL_C_INT_STATUS_G4X (1u << 5)
#define DP_AUX_CHANNEL_B_INT_STATUS_G4X (1u << 4)
#define DP_AUX_CHANNEL_MASK_INT_STATUS_G4X (7u << 4)
#define SDVOC_HOTPLUG_INT_STATUS_G4X (1u << 3)
#define SDVOB_HOTPLUG_INT_STATUS_G4X (1u << 2)
#define SDVOC_HOTPLUG_INT_STATUS_I915 (1u << 7)
#define SDVOB_HOTPLUG_INT_STATUS_I915 (1u << 6)
#define HOTPLUG_INT_STATUS_G4X (CRT_HOTPLUG_INT_STATUS | SDVOB_HOTPLUG_INT_STATUS_G4X |  \
				SDVOC_HOTPLUG_INT_STATUS_G4X | PORTB_HOTPLUG_INT_STATUS | \
				PORTC_HOTPLUG_INT_STATUS | PORTD_HOTPLUG_INT_STATUS)
#define HOTPLUG_INT_STATUS_I915 (CRT_HOTPLUG_INT_STATUS | SDVOB_HOTPLUG_INT_STATUS_I915 | \
				 SDVOC_HOTPLUG_INT_STATUS_I915 | PORTB_HOTPLUG_INT_STATUS | \
				 PORTC_HOTPLUG_INT_STATUS | PORTD_HOTPLUG_INT_STATUS)
/* G45: the PEG band gap needs a nudge before hotplug detection works */
#define PEG_BAND_GAP_DATA 0x14d68u

/* PCH (Ibex Peak and Cougar/Panther Point) */
#define PCH_PORT_HOTPLUG 0xc4030u
#define PORTD_HOTPLUG_ENABLE (1u << 20)
#define PORTD_PULSE_DURATION_2ms (0u << 18)
#define PORTD_PULSE_DURATION_MASK (3u << 18)
#define PORTD_HOTPLUG_STATUS_MASK (3u << 16)
#define PORTC_HOTPLUG_ENABLE (1u << 12)
#define PORTC_PULSE_DURATION_2ms (0u << 10)
#define PORTC_PULSE_DURATION_MASK (3u << 10)
#define PORTC_HOTPLUG_STATUS_MASK (3u << 8)
#define PORTB_HOTPLUG_ENABLE (1u << 4)
#define PORTB_PULSE_DURATION_2ms (0u << 2)
#define PORTB_PULSE_DURATION_MASK (3u << 2)
#define PORTB_HOTPLUG_STATUS_MASK (3u << 0)

/* ---- the ports ---------------------------------------------------------------- */

/* Analog (VGA) DAC */
#define ADPA 0x61100u
#define PCH_ADPA 0xe1100u
#define ADPA_DAC_ENABLE (1u << 31)
#define ADPA_PIPE_SEL_MASK (1u << 30)
#define ADPA_PIPE_SEL(p) ((uint32_t)(p) << 30)
#define ADPA_PIPE_SEL_MASK_CPT (3u << 29)
#define ADPA_PIPE_SEL_CPT(p) ((uint32_t)(p) << 29)
#define ADPA_CRT_HOTPLUG_MONITOR_MASK (3u << 24)
#define ADPA_CRT_HOTPLUG_MONITOR_NONE (0u << 24)
#define ADPA_CRT_HOTPLUG_MONITOR_COLOR (3u << 24)
#define ADPA_CRT_HOTPLUG_MONITOR_MONO (2u << 24)
#define ADPA_CRT_HOTPLUG_ENABLE (1u << 23)
#define ADPA_CRT_HOTPLUG_PERIOD_64 (0u << 22)
#define ADPA_CRT_HOTPLUG_PERIOD_128 (1u << 22)
#define ADPA_CRT_HOTPLUG_WARMUP_5MS (0u << 21)
#define ADPA_CRT_HOTPLUG_WARMUP_10MS (1u << 21)
#define ADPA_CRT_HOTPLUG_SAMPLE_2S (0u << 20)
#define ADPA_CRT_HOTPLUG_SAMPLE_4S (1u << 20)
#define ADPA_CRT_HOTPLUG_VOLTAGE_40 (0u << 18)
#define ADPA_CRT_HOTPLUG_VOLTAGE_50 (1u << 18)
#define ADPA_CRT_HOTPLUG_VOLTAGE_60 (2u << 18)
#define ADPA_CRT_HOTPLUG_VOLTAGE_70 (3u << 18)
#define ADPA_CRT_HOTPLUG_VOLREF_325MV (0u << 17)
#define ADPA_CRT_HOTPLUG_VOLREF_475MV (1u << 17)
#define ADPA_CRT_HOTPLUG_FORCE_TRIGGER (1u << 16)
#define ADPA_USE_VGA_HVPOLARITY (1u << 15)
#define ADPA_HSYNC_CNTL_DISABLE (1u << 11)
#define ADPA_VSYNC_CNTL_DISABLE (1u << 10)
#define ADPA_VSYNC_ACTIVE_HIGH (1u << 4)
#define ADPA_HSYNC_ACTIVE_HIGH (1u << 3)

/* LVDS */
#define LVDS 0x61180u
#define PCH_LVDS 0xe1180u
#define LVDS_PORT_EN (1u << 31)
#define LVDS_PIPE_SEL_MASK (1u << 30)
#define LVDS_PIPE_SEL(p) ((uint32_t)(p) << 30)
#define LVDS_PIPE_SEL_MASK_CPT (3u << 29)
#define LVDS_PIPE_SEL_CPT(p) ((uint32_t)(p) << 29)
#define LVDS_ENABLE_DITHER (1u << 25)
#define LVDS_VSYNC_POLARITY (1u << 21)
#define LVDS_HSYNC_POLARITY (1u << 20)
#define LVDS_BORDER_ENABLE (1u << 15)
#define LVDS_A0A2_CLKA_POWER_MASK (3u << 8)
#define LVDS_A0A2_CLKA_POWER_DOWN (0u << 8)
#define LVDS_A0A2_CLKA_POWER_UP (3u << 8)
#define LVDS_A3_POWER_MASK (3u << 6)
#define LVDS_A3_POWER_DOWN (0u << 6)
#define LVDS_A3_POWER_UP (3u << 6)
#define LVDS_CLKB_POWER_MASK (3u << 4)
#define LVDS_CLKB_POWER_DOWN (0u << 4)
#define LVDS_CLKB_POWER_UP (3u << 4)
#define LVDS_B0B3_POWER_MASK (3u << 2)
#define LVDS_B0B3_POWER_DOWN (0u << 2)
#define LVDS_B0B3_POWER_UP (3u << 2)
#define LVDS_DETECTED (1u << 1)

/* SDVO and HDMI share a register per port. */
#define GEN3_SDVOB 0x61140u
#define GEN3_SDVOC 0x61160u
#define GEN4_HDMIB GEN3_SDVOB
#define GEN4_HDMIC GEN3_SDVOC
#define CHV_HDMID 0x6116cu /* display-relative */
#define PCH_SDVOB 0xe1140u
#define PCH_HDMIB PCH_SDVOB
#define PCH_HDMIC 0xe1150u
#define PCH_HDMID 0xe1160u
#define PORT_DFT_I9XX 0x61150u
#define DC_BALANCE_RESET (1u << 25)
#define PORT_DFT2_G4X 0x61154u
#define DC_BALANCE_RESET_VLV (1u << 31)
#define PIPE_SCRAMBLE_RESET_MASK ((1u << 14) | (0x3u << 0))
#define SDVO_ENABLE (1u << 31)
#define SDVO_PIPE_SEL_SHIFT 30
#define SDVO_PIPE_SEL_MASK (1u << 30)
#define SDVO_PIPE_SEL(p) ((uint32_t)(p) << 30)
#define SDVO_STALL_SELECT (1u << 29)
#define SDVO_INTERRUPT_ENABLE (1u << 26)
#define SDVO_PORT_MULTIPLY_MASK (7u << 23)
#define SDVO_PORT_MULTIPLY_SHIFT 23
#define SDVO_PHASE_SELECT_MASK (15u << 19)
#define SDVO_PHASE_SELECT_DEFAULT (6u << 19)
#define SDVO_CLOCK_OUTPUT_INVERT (1u << 18)
#define SDVOC_GANG_MODE (1u << 16)
#define SDVO_BORDER_ENABLE (1u << 7)
#define SDVOB_PCIE_CONCURRENCY (1u << 3)
#define SDVO_DETECTED (1u << 2)
#define SDVOB_PRESERVE_MASK ((1u << 17) | (1u << 16) | (1u << 14) | SDVO_INTERRUPT_ENABLE)
#define SDVOC_PRESERVE_MASK ((1u << 17) | SDVO_INTERRUPT_ENABLE)
#define SDVO_COLOR_FORMAT_8bpc (0u << 26)
#define SDVO_COLOR_FORMAT_MASK (7u << 26)
#define SDVO_ENCODING_SDVO (0u << 10)
#define SDVO_ENCODING_HDMI (2u << 10)
#define HDMI_MODE_SELECT_HDMI (1u << 9)
#define HDMI_MODE_SELECT_DVI (0u << 9)
#define HDMI_COLOR_RANGE_16_235 (1u << 8)
#define HDMI_AUDIO_ENABLE (1u << 6)
#define SDVO_VSYNC_ACTIVE_HIGH (1u << 4)
#define SDVO_HSYNC_ACTIVE_HIGH (1u << 3)
#define HDMI_COLOR_FORMAT_12bpc (3u << 26)
#define SDVOB_HOTPLUG_ENABLE (1u << 23)
#define SDVO_PIPE_SEL_SHIFT_CPT 29
#define SDVO_PIPE_SEL_MASK_CPT (3u << 29)
#define SDVO_PIPE_SEL_CPT(p) ((uint32_t)(p) << 29)
#define SDVO_PIPE_SEL_SHIFT_CHV 24
#define SDVO_PIPE_SEL_MASK_CHV (3u << 24)
#define SDVO_PIPE_SEL_CHV(p) ((uint32_t)(p) << 24)

/* DisplayPort */
#define DP_A 0x64000u /* CPU eDP, Ironlake to Ivy Bridge */
#define DP_B 0x64100u
#define DP_C 0x64200u
#define DP_D 0x64300u
#define PCH_DP_B 0xe4100u
#define PCH_DP_C 0xe4200u
#define PCH_DP_D 0xe4300u
#define DP_PORT_EN (1u << 31)
#define DP_PIPE_SEL_MASK (1u << 30)
#define DP_PIPE_SEL(p) ((uint32_t)(p) << 30)
#define DP_PIPE_SEL_MASK_IVB (3u << 29)
#define DP_PIPE_SEL_IVB(p) ((uint32_t)(p) << 29)
#define DP_PIPE_SEL_SHIFT_CHV 16
#define DP_PIPE_SEL_MASK_CHV (3u << 16)
#define DP_PIPE_SEL_CHV(p) ((uint32_t)(p) << 16)
#define DP_LINK_TRAIN_MASK (3u << 28)
#define DP_LINK_TRAIN_PAT_1 (0u << 28)
#define DP_LINK_TRAIN_PAT_2 (1u << 28)
#define DP_LINK_TRAIN_PAT_IDLE (2u << 28)
#define DP_LINK_TRAIN_OFF (3u << 28)
#define DP_LINK_TRAIN_MASK_CPT (7u << 8)
#define DP_LINK_TRAIN_PAT_1_CPT (0u << 8)
#define DP_LINK_TRAIN_PAT_2_CPT (1u << 8)
#define DP_LINK_TRAIN_PAT_IDLE_CPT (2u << 8)
#define DP_LINK_TRAIN_OFF_CPT (3u << 8)
#define DP_VOLTAGE_MASK (7u << 25)
#define DP_VOLTAGE_0_4 (0u << 25)
#define DP_VOLTAGE_0_6 (1u << 25)
#define DP_VOLTAGE_0_8 (2u << 25)
#define DP_VOLTAGE_1_2 (3u << 25)
#define DP_PRE_EMPHASIS_MASK (7u << 22)
#define DP_PRE_EMPHASIS_0 (0u << 22)
#define DP_PRE_EMPHASIS_3_5 (1u << 22)
#define DP_PRE_EMPHASIS_6 (2u << 22)
#define DP_PRE_EMPHASIS_9_5 (3u << 22)
#define DP_PORT_WIDTH_MASK (7u << 19)
#define DP_PORT_WIDTH(w) (((uint32_t)(w) - 1u) << 19)
#define DP_ENHANCED_FRAMING (1u << 18)
#define EDP_PLL_FREQ_MASK (3u << 16)
#define EDP_PLL_FREQ_270MHZ (0u << 16)
#define EDP_PLL_FREQ_162MHZ (1u << 16)
#define DP_PORT_REVERSAL (1u << 15)
#define EDP_PLL_ENABLE (1u << 14)
#define DP_CLOCK_OUTPUT_ENABLE (1u << 13)
#define DP_SCRAMBLING_DISABLE (1u << 12)
#define DP_SCRAMBLING_DISABLE_ILK (1u << 7)
#define DP_COLOR_RANGE_16_235 (1u << 8)
#define DP_AUDIO_OUTPUT_ENABLE (1u << 6)
#define DP_SYNC_VS_HIGH (1u << 4)
#define DP_SYNC_HS_HIGH (1u << 3)
#define DP_DETECTED (1u << 2)
/* Sandy Bridge port A: one field for both swing and emphasis */
#define EDP_LINK_TRAIN_400_600MV_0DB_SNB_B (0x0u << 22)
#define EDP_LINK_TRAIN_400MV_3_5DB_SNB_B (0x1u << 22)
#define EDP_LINK_TRAIN_400_600MV_6DB_SNB_B (0x3au << 22)
#define EDP_LINK_TRAIN_600_800MV_3_5DB_SNB_B (0x39u << 22)
#define EDP_LINK_TRAIN_800_1200MV_0DB_SNB_B (0x38u << 22)
#define EDP_LINK_TRAIN_VOL_EMP_MASK_SNB (0x3fu << 22)
/* Ivy Bridge port A */
#define EDP_LINK_TRAIN_400MV_0DB_IVB (0x24u << 22)
#define EDP_LINK_TRAIN_400MV_3_5DB_IVB (0x2au << 22)
#define EDP_LINK_TRAIN_400MV_6DB_IVB (0x2fu << 22)
#define EDP_LINK_TRAIN_600MV_0DB_IVB (0x30u << 22)
#define EDP_LINK_TRAIN_600MV_3_5DB_IVB (0x36u << 22)
#define EDP_LINK_TRAIN_800MV_0DB_IVB (0x38u << 22)
#define EDP_LINK_TRAIN_800MV_3_5DB_IVB (0x3eu << 22)
#define EDP_LINK_TRAIN_500MV_0DB_IVB (0x00u << 22)
#define EDP_LINK_TRAIN_VOL_EMP_MASK_IVB (0x3fu << 22)

/* DVO (gen2): A, B and C */
#define DVOA 0x61120u
#define DVOB 0x61140u
#define DVOC 0x61160u
#define DVO_REG(port) (0x61120u + (uint32_t)(port) * 0x20u)
#define DVO_ENABLE (1u << 31)
#define DVO_PIPE_SEL_MASK (1u << 30)
#define DVO_PIPE_SEL(p) ((uint32_t)(p) << 30)
#define DVO_PIPE_STALL_MASK (3u << 28)
#define DVO_PIPE_STALL_UNUSED (0u << 28)
#define DVO_PIPE_STALL (1u << 28)
#define DVO_PIPE_STALL_TV (2u << 28)
#define DVO_INTERRUPT_SELECT (1u << 27)
#define DVO_DEDICATED_INT_ENABLE (1u << 26)
#define DVO_PRESERVE_MASK (3u << 24)
#define DVO_USE_VGA_SYNC (1u << 15)
#define DVO_DATA_ORDER_I740 (0u << 14)
#define DVO_DATA_ORDER_FP (1u << 14)
#define DVO_VSYNC_DISABLE (1u << 11)
#define DVO_HSYNC_DISABLE (1u << 10)
#define DVO_VSYNC_TRISTATE (1u << 9)
#define DVO_HSYNC_TRISTATE (1u << 8)
#define DVO_BORDER_ENABLE (1u << 7)
#define DVO_ACT_DATA_ORDER_GBRG_ERRATA (0u << 6)
#define DVO_ACT_DATA_ORDER_RGGB_ERRATA (1u << 6)
#define DVO_VSYNC_ACTIVE_HIGH (1u << 4)
#define DVO_HSYNC_ACTIVE_HIGH (1u << 3)
#define DVO_BLANK_ACTIVE_HIGH (1u << 2)
#define DVO_OUTPUT_CSTATE_PIXELS (1u << 1)
#define DVO_OUTPUT_SOURCE_SIZE_PIXELS (1u << 0)
#define DVO_SRCDIM(port) (0x61124u + (uint32_t)(port) * 0x20u)
#define DVO_SRCDIM_HORIZONTAL(x) ((uint32_t)(x) << 12)
#define DVO_SRCDIM_VERTICAL(x) ((uint32_t)(x))

/* ---- panel power sequencer ------------------------------------------------------- */

/* `base' is 0x61200 on the GMCH parts, 0xc7200 with a PCH, and
 * 0x61200 + 0x100 * pipe (display-relative) on Valleyview/Cherryview. */
#define PPS_BASE_GMCH 0x61200u
#define PPS_BASE_PCH 0xc7200u
#define PP_STATUS(base) ((base) + 0x00u)
#define PP_ON (1u << 31)
#define PP_READY (1u << 30)
#define PP_SEQUENCE_MASK (3u << 28)
#define PP_SEQUENCE_NONE (0u << 28)
#define PP_SEQUENCE_POWER_UP (1u << 28)
#define PP_SEQUENCE_POWER_DOWN (2u << 28)
#define PP_CYCLE_DELAY_ACTIVE (1u << 27)
#define PP_SEQUENCE_STATE_MASK 0xfu
#define PP_SEQUENCE_STATE_OFF_IDLE 0x0u
#define PP_SEQUENCE_STATE_ON_IDLE 0x8u
#define PP_SEQUENCE_STATE_RESET 0xfu
#define PP_CONTROL(base) ((base) + 0x04u)
#define PANEL_UNLOCK_MASK (0xffffu << 16)
#define PANEL_UNLOCK_REGS (0xabcdu << 16)
#define EDP_FORCE_VDD (1u << 3)
#define EDP_BLC_ENABLE (1u << 2)
#define PANEL_POWER_RESET (1u << 1)
#define PANEL_POWER_ON (1u << 0)
#define PP_ON_DELAYS(base) ((base) + 0x08u)
#define PANEL_PORT_SELECT_MASK (3u << 30)
#define PANEL_PORT_SELECT_LVDS (0u << 30)
#define PANEL_PORT_SELECT_DPA (1u << 30)
#define PANEL_PORT_SELECT_DPC (2u << 30)
#define PANEL_PORT_SELECT_DPD (3u << 30)
#define PANEL_PORT_SELECT_VLV(port) ((uint32_t)(port) << 30)
#define PANEL_POWER_UP_DELAY_MASK (0x1fffu << 16)
#define PANEL_POWER_UP_DELAY_SHIFT 16
#define PANEL_LIGHT_ON_DELAY_MASK 0x1fffu
#define PP_OFF_DELAYS(base) ((base) + 0x0cu)
#define PANEL_POWER_DOWN_DELAY_MASK (0x1fffu << 16)
#define PANEL_POWER_DOWN_DELAY_SHIFT 16
#define PANEL_LIGHT_OFF_DELAY_MASK 0x1fffu
#define PP_DIVISOR(base) ((base) + 0x10u)
#define PP_REFERENCE_DIVIDER_MASK (0xffffffu << 8)
#define PP_REFERENCE_DIVIDER_SHIFT 8
#define PANEL_POWER_CYCLE_DELAY_MASK 0x1fu

/* ---- backlight ---------------------------------------------------------------- */

#define BLC_PWM_CTL2 0x61250u /* 965+ */
#define BLM_PWM_ENABLE (1u << 31)
#define BLM_COMBINATION_MODE (1u << 30) /* gen4 */
#define BLM_PIPE_SELECT (1u << 29)
#define BLM_PIPE_SELECT_IVB (3u << 29)
#define BLM_PIPE(p) ((uint32_t)(p) << 29)
#define BLM_POLARITY_I965 (1u << 28)
#define BLM_PHASE_IN_INTERUPT_STATUS (1u << 26)
#define BLM_PHASE_IN_ENABLE (1u << 25)
#define BLM_PHASE_IN_INTERUPT_ENABL (1u << 24)
#define BLC_PWM_CTL 0x61254u
#define BACKLIGHT_MODULATION_FREQ_SHIFT 17
#define BACKLIGHT_MODULATION_FREQ_MASK (0x7fffu << 17)
#define BLM_LEGACY_MODE (1u << 16) /* gen2 */
#define BACKLIGHT_DUTY_CYCLE_SHIFT 0
#define BACKLIGHT_DUTY_CYCLE_MASK 0xffffu
#define BACKLIGHT_DUTY_CYCLE_MASK_PNV 0xfffeu
#define BLM_POLARITY_PNV (1u << 0)
#define BLC_HIST_CTL 0x61260u
#define BLM_HISTOGRAM_ENABLE (1u << 31)
/* Valleyview/Cherryview: one controller per pipe (display-relative) */
#define VLV_BLC_PWM_CTL2(p) (0x61250u + (uint32_t)(p) * 0x100u)
#define VLV_BLC_PWM_CTL(p) (0x61254u + (uint32_t)(p) * 0x100u)
#define VLV_BLC_HIST_CTL(p) (0x61260u + (uint32_t)(p) * 0x100u)
/* Ironlake on: the CPU half and the PCH half */
#define BLC_PWM_CPU_CTL2 0x48250u
#define BLC_PWM_CPU_CTL 0x48254u
#define BLC_PWM_PCH_CTL1 0xc8250u
#define BLM_PCH_PWM_ENABLE (1u << 31)
#define BLM_PCH_OVERRIDE_ENABLE (1u << 30)
#define BLM_PCH_POLARITY (1u << 29)
#define BLC_PWM_PCH_CTL2 0xc8254u

/* ---- panel fitters ------------------------------------------------------------- */

/* GMCH (one, shared by the pipes) and Valleyview/Cherryview */
#define PFIT_CONTROL 0x61230u
#define PFIT_ENABLE (1u << 31)
#define PFIT_PIPE_MASK (3u << 29)
#define PFIT_PIPE(p) ((uint32_t)(p) << 29)
#define PFIT_SCALING_MASK (7u << 26)
#define PFIT_SCALING_AUTO (0u << 26)
#define PFIT_SCALING_PROGRAMMED (1u << 26)
#define PFIT_SCALING_PILLAR (2u << 26)
#define PFIT_SCALING_LETTER (3u << 26)
#define PFIT_FILTER_MASK (3u << 24)
#define PFIT_FILTER_FUZZY (0u << 24)
#define PFIT_FILTER_CRISP (1u << 24)
#define PFIT_FILTER_MEDIAN (2u << 24)
#define PFIT_VERT_INTERP_MASK (3u << 10)
#define PFIT_VERT_INTERP_BILINEAR (1u << 10)
#define PFIT_VERT_AUTO_SCALE (1u << 9)
#define PFIT_HORIZ_INTERP_MASK (3u << 6)
#define PFIT_HORIZ_INTERP_BILINEAR (1u << 6)
#define PFIT_HORIZ_AUTO_SCALE (1u << 5)
#define PFIT_PANEL_8TO6_DITHER_ENABLE (1u << 3)
#define PFIT_PGM_RATIOS 0x61234u
#define PFIT_VERT_SCALE(x) ((uint32_t)(x) << 20) /* pre-965, 12 bits */
#define PFIT_HORIZ_SCALE(x) ((uint32_t)(x) << 4) /* pre-965, 12 bits */
#define PFIT_VERT_SCALE_MASK_965 (0x1fffu << 16)
#define PFIT_HORIZ_SCALE_MASK_965 0x1fffu
#define PFIT_AUTO_RATIOS 0x61238u
/* Ironlake on: one per pipe (0x800 apart) */
#define PF_CTL(p) (0x68080u + (uint32_t)(p) * 0x800u)
#define PF_ENABLE (1u << 31)
#define PF_PIPE_SEL_MASK_IVB (3u << 29)
#define PF_PIPE_SEL_IVB(p) ((uint32_t)(p) << 29)
#define PF_FILTER_MASK (3u << 23)
#define PF_FILTER_PROGRAMMED (0u << 23)
#define PF_FILTER_MED_3x3 (1u << 23)
#define PF_WIN_SZ(p) (0x68074u + (uint32_t)(p) * 0x800u)
#define PF_WIN_POS(p) (0x68070u + (uint32_t)(p) * 0x800u)
#define PF_VSCALE(p) (0x68084u + (uint32_t)(p) * 0x800u)
#define PF_HSCALE(p) (0x68090u + (uint32_t)(p) * 0x800u)

/* ---- FDI and the PCH transcoders (Ironlake to Ivy Bridge) ---------------------- */

#define FDI_PLL_BIOS_0 0x46000u
#define FDI_PLL_FB_CLOCK_MASK 0xffu
#define FDI_PLL_FREQ_CTL 0x46030u
#define FDI_RX_CHICKEN(p) (0xc200cu + (uint32_t)(p) * 4u)
#define FDI_RX_PHASE_SYNC_POINTER_OVR (1u << 1)
#define FDI_RX_PHASE_SYNC_POINTER_EN (1u << 0)
#define FDI_TX_CTL(p) (0x60100u + (uint32_t)(p) * 0x1000u)
#define FDI_TX_DISABLE (0u << 31)
#define FDI_TX_ENABLE (1u << 31)
#define FDI_LINK_TRAIN_PATTERN_1 (0u << 28)
#define FDI_LINK_TRAIN_PATTERN_2 (1u << 28)
#define FDI_LINK_TRAIN_PATTERN_IDLE (2u << 28)
#define FDI_LINK_TRAIN_NONE (3u << 28)
#define FDI_LINK_TRAIN_VOLTAGE_0_4V (0u << 25)
#define FDI_LINK_TRAIN_VOLTAGE_0_6V (1u << 25)
#define FDI_LINK_TRAIN_VOLTAGE_0_8V (2u << 25)
#define FDI_LINK_TRAIN_VOLTAGE_1_2V (3u << 25)
#define FDI_LINK_TRAIN_PRE_EMPHASIS_NONE (0u << 22)
#define FDI_LINK_TRAIN_PRE_EMPHASIS_1_5X (1u << 22)
#define FDI_LINK_TRAIN_PRE_EMPHASIS_2X (2u << 22)
#define FDI_LINK_TRAIN_PRE_EMPHASIS_3X (3u << 22)
#define FDI_LINK_TRAIN_400MV_0DB_SNB_A (0x38u << 22)
#define FDI_LINK_TRAIN_400MV_6DB_SNB_A (0x02u << 22)
#define FDI_LINK_TRAIN_600MV_3_5DB_SNB_A (0x01u << 22)
#define FDI_LINK_TRAIN_800MV_0DB_SNB_A (0x0u << 22)
#define FDI_LINK_TRAIN_400MV_0DB_SNB_B (0x0u << 22)
#define FDI_LINK_TRAIN_400MV_6DB_SNB_B (0x3au << 22)
#define FDI_LINK_TRAIN_600MV_3_5DB_SNB_B (0x39u << 22)
#define FDI_LINK_TRAIN_800MV_0DB_SNB_B (0x38u << 22)
#define FDI_LINK_TRAIN_VOL_EMP_MASK (0x3fu << 22)
#define FDI_DP_PORT_WIDTH_SHIFT 19
#define FDI_DP_PORT_WIDTH_MASK (7u << 19)
#define FDI_DP_PORT_WIDTH(w) (((uint32_t)(w) - 1u) << 19)
#define FDI_TX_ENHANCE_FRAME_ENABLE (1u << 18)
#define FDI_TX_PLL_ENABLE (1u << 14)
#define FDI_LINK_TRAIN_PATTERN_1_IVB (0u << 8)
#define FDI_LINK_TRAIN_PATTERN_2_IVB (1u << 8)
#define FDI_LINK_TRAIN_PATTERN_IDLE_IVB (2u << 8)
#define FDI_LINK_TRAIN_NONE_IVB (3u << 8)
#define FDI_COMPOSITE_SYNC (1u << 11)
#define FDI_LINK_TRAIN_AUTO (1u << 10)
#define FDI_SCRAMBLING_ENABLE (0u << 7)
#define FDI_SCRAMBLING_DISABLE (1u << 7)
#define FDI_RX_CTL(p) (0xf000cu + (uint32_t)(p) * 0x1000u)
#define FDI_RX_ENABLE (1u << 31)
#define FDI_FS_ERRC_ENABLE (1u << 27)
#define FDI_FE_ERRC_ENABLE (1u << 26)
#define FDI_8BPC (0u << 16)
#define FDI_10BPC (1u << 16)
#define FDI_6BPC (2u << 16)
#define FDI_12BPC (3u << 16)
#define FDI_BPC_MASK (7u << 16)
#define FDI_RX_LINK_REVERSAL_OVERRIDE (1u << 15)
#define FDI_DMI_LINK_REVERSE_MASK (1u << 14)
#define FDI_RX_PLL_ENABLE (1u << 13)
#define FDI_FS_ERR_CORRECT_ENABLE (1u << 11)
#define FDI_FE_ERR_CORRECT_ENABLE (1u << 10)
#define FDI_FS_ERR_REPORT_ENABLE (1u << 9)
#define FDI_FE_ERR_REPORT_ENABLE (1u << 8)
#define FDI_RX_ENHANCE_FRAME_ENABLE (1u << 6)
#define FDI_PCDCLK (1u << 4)
#define FDI_AUTO_TRAINING (1u << 10)
#define FDI_LINK_TRAIN_PATTERN_1_CPT (0u << 8)
#define FDI_LINK_TRAIN_PATTERN_2_CPT (1u << 8)
#define FDI_LINK_TRAIN_PATTERN_IDLE_CPT (2u << 8)
#define FDI_LINK_TRAIN_NORMAL_CPT (3u << 8)
#define FDI_LINK_TRAIN_PATTERN_MASK_CPT (3u << 8)
#define FDI_RX_MISC(p) (0xf0010u + (uint32_t)(p) * 0x1000u)
#define FDI_RX_PWRDN_LANE1_MASK (3u << 26)
#define FDI_RX_PWRDN_LANE1_VAL(x) ((uint32_t)(x) << 26)
#define FDI_RX_PWRDN_LANE0_MASK (3u << 24)
#define FDI_RX_PWRDN_LANE0_VAL(x) ((uint32_t)(x) << 24)
#define FDI_RX_TP1_TO_TP2_48 (2u << 20)
#define FDI_RX_TP1_TO_TP2_64 (3u << 20)
#define FDI_RX_FDI_DELAY_90 (0x90u << 0)
#define FDI_RX_TUSIZE1(p) (0xf0030u + (uint32_t)(p) * 0x1000u)
#define FDI_RX_TUSIZE2(p) (0xf0038u + (uint32_t)(p) * 0x1000u)
#define FDI_RX_IIR(p) (0xf0014u + (uint32_t)(p) * 0x1000u)
#define FDI_RX_IMR(p) (0xf0018u + (uint32_t)(p) * 0x1000u)
#define FDI_RX_INTER_LANE_ALIGN (1u << 10)
#define FDI_RX_SYMBOL_LOCK (1u << 9)
#define FDI_RX_BIT_LOCK (1u << 8)
#define FDI_RX_TRAIN_PATTERN_2_FAIL (1u << 7)
#define FDI_PLL_CTL_1 0xfe000u
#define FDI_PLL_CTL_2 0xfe004u

#define PCH_DPLL(i) (0xc6014u + (uint32_t)(i) * 4u)
#define PCH_FP0(i) (0xc6040u + (uint32_t)(i) * 8u)
#define PCH_FP1(i) (0xc6044u + (uint32_t)(i) * 8u)
#define PCH_DPLL_TEST 0xc606cu
#define PCH_DREF_CONTROL 0xc6200u
#define DREF_CONTROL_MASK 0x7fc3u
#define DREF_CPU_SOURCE_OUTPUT_DISABLE (0u << 13)
#define DREF_CPU_SOURCE_OUTPUT_DOWNSPREAD (2u << 13)
#define DREF_CPU_SOURCE_OUTPUT_NONSPREAD (3u << 13)
#define DREF_CPU_SOURCE_OUTPUT_MASK (3u << 13)
#define DREF_SSC_SOURCE_DISABLE (0u << 11)
#define DREF_SSC_SOURCE_ENABLE (2u << 11)
#define DREF_SSC_SOURCE_MASK (3u << 11)
#define DREF_NONSPREAD_SOURCE_DISABLE (0u << 9)
#define DREF_NONSPREAD_CK505_ENABLE (1u << 9)
#define DREF_NONSPREAD_SOURCE_ENABLE (2u << 9)
#define DREF_NONSPREAD_SOURCE_MASK (3u << 9)
#define DREF_SUPERSPREAD_SOURCE_DISABLE (0u << 7)
#define DREF_SUPERSPREAD_SOURCE_ENABLE (2u << 7)
#define DREF_SUPERSPREAD_SOURCE_MASK (3u << 7)
#define DREF_SSC4_DOWNSPREAD (0u << 6)
#define DREF_SSC4_CENTERSPREAD (1u << 6)
#define DREF_SSC1_DISABLE (0u << 1)
#define DREF_SSC1_ENABLE (1u << 1)
#define DREF_SSC4_DISABLE (0u)
#define DREF_SSC4_ENABLE (1u)
#define PCH_RAWCLK_FREQ 0xc6204u
#define FDL_TP1_TIMER_SHIFT 12
#define FDL_TP1_TIMER_MASK (3u << 12)
#define FDL_TP2_TIMER_SHIFT 10
#define FDL_TP2_TIMER_MASK (3u << 10)
#define RAWCLK_FREQ_MASK 0x3ffu
#define PCH_DPLL_TMR_CFG 0xc6208u
#define PCH_SSC4_PARMS 0xc6210u
#define PCH_SSC4_AUX_PARMS 0xc6214u
#define PCH_DPLL_SEL 0xc7000u
#define TRANS_DPLLB_SEL(p) (1u << ((p) * 4))
#define TRANS_DPLLA_SEL(p) 0u
#define TRANS_DPLL_ENABLE(p) (1u << ((p) * 4 + 3))

#define PCH_TRANS_HTOTAL(p) (0xe0000u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_HBLANK(p) (0xe0004u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_HSYNC(p) (0xe0008u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_VTOTAL(p) (0xe000cu + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_VBLANK(p) (0xe0010u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_VSYNC(p) (0xe0014u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_VSYNCSHIFT(p) (0xe0028u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_DATA_M1(p) (0xe0030u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_DATA_N1(p) (0xe0034u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_DATA_M2(p) (0xe0038u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_DATA_N2(p) (0xe003cu + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_LINK_M1(p) (0xe0040u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_LINK_N1(p) (0xe0044u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_LINK_M2(p) (0xe0048u + (uint32_t)(p) * 0x1000u)
#define PCH_TRANS_LINK_N2(p) (0xe004cu + (uint32_t)(p) * 0x1000u)
#define PCH_TRANSCONF(p) (0xf0008u + (uint32_t)(p) * 0x1000u)
#define TRANS_ENABLE (1u << 31)
#define TRANS_STATE_ENABLE (1u << 30)
#define TRANS_FRAME_START_DELAY_MASK (3u << 27)
#define TRANS_FRAME_START_DELAY(x) ((uint32_t)(x) << 27)
#define TRANS_INTERLACE_MASK (7u << 21)
#define TRANS_INTERLACE_PROGRESSIVE (0u << 21)
#define TRANS_INTERLACE_LEGACY_VSYNC_IBX (2u << 21)
#define TRANS_INTERLACE_INTERLACED (3u << 21)
#define TRANS_BPC_MASK (7u << 5)
#define TRANS_BPC_8 (0u << 5)
#define TRANS_BPC_10 (1u << 5)
#define TRANS_BPC_6 (2u << 5)
#define TRANS_BPC_12 (3u << 5)
#define TRANS_CHICKEN1(p) (0xf0060u + (uint32_t)(p) * 0x1000u)
#define TRANS_CHICKEN1_HDMIUNIT_GC_DISABLE (1u << 10)
#define TRANS_CHICKEN1_DP0UNIT_GC_DISABLE (1u << 4)
#define TRANS_CHICKEN2(p) (0xf0064u + (uint32_t)(p) * 0x1000u)
#define TRANS_CHICKEN2_TIMING_OVERRIDE (1u << 31)
#define TRANS_CHICKEN2_FDI_POLARITY_REVERSED (1u << 29)
#define TRANS_CHICKEN2_FRAME_START_DELAY_MASK (3u << 27)
#define TRANS_CHICKEN2_FRAME_START_DELAY(x) ((uint32_t)(x) << 27)
#define TRANS_CHICKEN2_DISABLE_DEEP_COLOR_COUNTER (1u << 26)
#define TRANS_CHICKEN2_DISABLE_DEEP_COLOR_MODESWITCH (1u << 25)
/* Cougar Point: the transcoder's DisplayPort control */
#define TRANS_DP_CTL(p) (0xe0300u + (uint32_t)(p) * 0x1000u)
#define TRANS_DP_OUTPUT_ENABLE (1u << 31)
#define TRANS_DP_PORT_SEL_MASK (3u << 29)
#define TRANS_DP_PORT_SEL_NONE (3u << 29)
#define TRANS_DP_PORT_SEL(port_minus_b) ((uint32_t)(port_minus_b) << 29)
#define TRANS_DP_AUDIO_ONLY (1u << 26)
#define TRANS_DP_ENH_FRAMING (1u << 18)
#define TRANS_DP_BPC_MASK (3u << 9)
#define TRANS_DP_BPC_8 (0u << 9)
#define TRANS_DP_BPC_10 (1u << 9)
#define TRANS_DP_BPC_6 (2u << 9)
#define TRANS_DP_BPC_12 (3u << 9)
#define TRANS_DP_VSYNC_ACTIVE_HIGH (1u << 4)
#define TRANS_DP_HSYNC_ACTIVE_HIGH (1u << 3)

#define SOUTH_CHICKEN1 0xc2000u
#define FDIA_PHASE_SYNC_SHIFT_OVR 19
#define FDIA_PHASE_SYNC_SHIFT_EN 18
#define FDI_PHASE_SYNC_OVR(p) (1u << (FDIA_PHASE_SYNC_SHIFT_OVR - ((p) * 2)))
#define FDI_PHASE_SYNC_EN(p) (1u << (FDIA_PHASE_SYNC_SHIFT_EN - ((p) * 2)))
#define FDI_BC_BIFURCATION_SELECT (1u << 12)
#define SOUTH_CHICKEN2 0xc2004u
#define DPLS_EDP_PPS_FIX_DIS (1u << 0)
#define SOUTH_DSPCLK_GATE_D 0xc2020u
#define PCH_GMBUSUNIT_CLOCK_GATE_DISABLE (1u << 31)
#define PCH_DPLUNIT_CLOCK_GATE_DISABLE (1u << 30)
#define PCH_DPLSUNIT_CLOCK_GATE_DISABLE (1u << 29)
#define PCH_CPUNIT_CLOCK_GATE_DISABLE (1u << 14)
#define SFUSE_STRAP 0xc2014u
#define SFUSE_STRAP_FUSE_LOCK (1u << 13)
#define SFUSE_STRAP_DISPLAY_DISABLED (1u << 7)
#define SFUSE_STRAP_CRT_DISABLED (1u << 6)
#define SFUSE_STRAP_DDIB_DETECTED (1u << 2)
#define SFUSE_STRAP_DDIC_DETECTED (1u << 1)
#define SFUSE_STRAP_DDID_DETECTED (1u << 0)

/* ---- north display chicken bits (Ironlake to Ivy Bridge) -------------------------- */

#define ILK_DISPLAY_CHICKEN1 0x42000u
#define ILK_FBCQ_DIS (1u << 22)
#define ILK_PABSTRETCH_DIS (1u << 21)
#define ILK_SABSTRETCH_DIS (1u << 20)
#define ILK_DISPLAY_CHICKEN2 0x42004u
#define ILK_ELPIN_409_SELECT (1u << 25)
#define ILK_DPARB_GATE (1u << 22)
#define ILK_VSDPFD_FULL (1u << 21)
#define FUSE_STRAP 0x42014u
#define ILK_INTERNAL_GRAPHICS_DISABLE (1u << 31)
#define ILK_INTERNAL_DISPLAY_DISABLE (1u << 30)
#define ILK_DISPLAY_DEBUG_DISABLE (1u << 29)
#define IVB_PIPE_C_DISABLE (1u << 28)
#define ILK_HDCP_DISABLE (1u << 25)
#define ILK_eDP_A_DISABLE (1u << 24)
#define ILK_DESKTOP (1u << 23)
#define ILK_DSPCLK_GATE_D 0x42020u
#define ILK_VRHUNIT_CLOCK_GATE_DISABLE (1u << 28)
#define ILK_DPFCUNIT_CLOCK_GATE_DISABLE (1u << 9)
#define ILK_DPFCRUNIT_CLOCK_GATE_DISABLE (1u << 8)
#define ILK_DPFDUNIT_CLOCK_GATE_ENABLE (1u << 7)
#define ILK_DPARBUNIT_CLOCK_GATE_ENABLE (1u << 5)
#define DISP_ARB_CTL 0x45000u
#define DISP_FBC_WM_DIS (1u << 15)

/* ---- miscellaneous GMCH registers ------------------------------------------------ */

#define MI_ARB_STATE 0x20e4u /* masked: write (bit << 16) | value */
#define MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE (1u << 2)
#define MEM_MODE 0x20ccu /* masked */
#define MEM_DISPLAY_B_TRICKLE_FEED_DISABLE (1u << 3)
#define MEM_DISPLAY_A_TRICKLE_FEED_DISABLE (1u << 2)
#define MEM_DISPLAY_TRICKLE_FEED_DISABLE (1u << 2)
#define LG_MASKED_ENABLE(bit) (((uint32_t)(bit) << 16) | (uint32_t)(bit))
#define LG_MASKED_DISABLE(bit) ((uint32_t)(bit) << 16)
/* Software flags the video BIOS keeps (scratch) */
#define SWF0(i) (0x70410u + (uint32_t)(i) * 4u)
#define SWF1(i) (0x71410u + (uint32_t)(i) * 4u)
#define SWF3(i) (0x72414u + (uint32_t)(i) * 4u)
#define SWF_ILK(i) (0x4f000u + (uint32_t)(i) * 4u)

/* ---- GMBUS and the GPIO pairs ---------------------------------------------------- */

/* Offsets from the GMBUS block: 0 on the GMCH parts (display-relative on
 * Valleyview/Cherryview), 0xc0000 with a PCH. */
#define GPIO_CTL(gbase, n) ((gbase) + 0x5010u + (uint32_t)(n) * 4u)
#define GPIO_CLOCK_DIR_MASK (1u << 0)
#define GPIO_CLOCK_DIR_IN (0u << 1)
#define GPIO_CLOCK_DIR_OUT (1u << 1)
#define GPIO_CLOCK_VAL_MASK (1u << 2)
#define GPIO_CLOCK_VAL_OUT (1u << 3)
#define GPIO_CLOCK_VAL_IN (1u << 4)
#define GPIO_CLOCK_PULLUP_DISABLE (1u << 5)
#define GPIO_DATA_DIR_MASK (1u << 8)
#define GPIO_DATA_DIR_IN (0u << 9)
#define GPIO_DATA_DIR_OUT (1u << 9)
#define GPIO_DATA_VAL_MASK (1u << 10)
#define GPIO_DATA_VAL_OUT (1u << 11)
#define GPIO_DATA_VAL_IN (1u << 12)
#define GPIO_DATA_PULLUP_DISABLE (1u << 13)
#define GMBUS0_REG(gbase) ((gbase) + 0x5100u)
#define GMBUS_AKSV_SELECT (1u << 11)
#define GMBUS_RATE_100KHZ (0u << 8)
#define GMBUS_RATE_50KHZ (1u << 8)
#define GMBUS_RATE_400KHZ (2u << 8)
#define GMBUS_RATE_1MHZ (3u << 8)
#define GMBUS_HOLD_EXT (1u << 7)
#define GMBUS_BYTE_CNT_OVERRIDE (1u << 6)
#define GMBUS1_REG(gbase) ((gbase) + 0x5104u)
#define GMBUS_SW_CLR_INT (1u << 31)
#define GMBUS_SW_RDY (1u << 30)
#define GMBUS_ENT (1u << 29)
#define GMBUS_CYCLE_NONE (0u << 25)
#define GMBUS_CYCLE_WAIT (1u << 25)
#define GMBUS_CYCLE_INDEX (2u << 25)
#define GMBUS_CYCLE_STOP (4u << 25)
#define GMBUS_BYTE_COUNT_SHIFT 16
#define GMBUS_BYTE_COUNT_MAX 256u
#define GMBUS_SLAVE_INDEX_SHIFT 8
#define GMBUS_SLAVE_ADDR_SHIFT 1
#define GMBUS_SLAVE_READ (1u << 0)
#define GMBUS_SLAVE_WRITE (0u << 0)
#define GMBUS2_REG(gbase) ((gbase) + 0x5108u)
#define GMBUS_INUSE (1u << 15)
#define GMBUS_HW_WAIT_PHASE (1u << 14)
#define GMBUS_STALL_TIMEOUT (1u << 13)
#define GMBUS_INT (1u << 12)
#define GMBUS_HW_RDY (1u << 11)
#define GMBUS_SATOER (1u << 10)
#define GMBUS_ACTIVE (1u << 9)
#define GMBUS3_REG(gbase) ((gbase) + 0x510cu)
#define GMBUS4_REG(gbase) ((gbase) + 0x5110u)
#define GMBUS_SLAVE_TIMEOUT_EN (1u << 4)
#define GMBUS_NAK_EN (1u << 3)
#define GMBUS_IDLE_EN (1u << 2)
#define GMBUS_HW_WAIT_EN (1u << 1)
#define GMBUS_HW_RDY_EN (1u << 0)
#define GMBUS5_REG(gbase) ((gbase) + 0x5120u)
#define GMBUS_2BYTE_INDEX_EN (1u << 31)

#endif
