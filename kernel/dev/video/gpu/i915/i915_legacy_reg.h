// LikeOS -- register offsets and command encodings of the Intel graphics
// generations before Broadwell (i830 to Haswell, Valleyview).
//
// The GT of these parts is driven by ring buffers instead of execution
// lists, its address spaces are the global GTT (and on Gen6/7 one
// two-level PPGTT), and much of what Gen8 moved into the GT's own
// register blocks still lives in the GMCH or the north display block.
// The names are the datasheets' names, the same ones the Gen8 header
// uses where a register did not move; this header is private to the
// i915_legacy_*.c files and is never included next to i915_reg.h.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2003-2024 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_LEGACY_REG_H
#define KERNEL_DEV_GPU_I915_LEGACY_REG_H

#ifdef KERNEL_DEV_GPU_I915_REG_H
#error "i915_legacy_reg.h names registers i915_reg.h also names; include only one"
#endif

#define LEG_BIT(n) (1u << (n))
#define LEG_MASKED_EN(bits) (((bits) << 16) | (bits))
#define LEG_MASKED_DIS(bits) ((bits) << 16)
#define LEG_MASKED_FIELD(mask, value) (((mask) << 16) | (value))

/* The status page dwords the driver uses: the sequence number every
 * breadcrumb writes (the same index the engine code reads) and a
 * scratch dword for flushes that must write something. */
#define LEG_HWS_SEQNO_INDEX 0x40
#define LEG_HWS_SEQNO_ADDR (LEG_HWS_SEQNO_INDEX * 4)
#define LEG_HWS_SCRATCH_INDEX 0x80
#define LEG_HWS_SCRATCH_ADDR (LEG_HWS_SCRATCH_INDEX * 4)

/* ---- PCI configuration: the graphics function and the host bridge ------- */

/* BARs per generation: gen2 has the aperture first and the registers
 * (with the GTT inside them) second; gen3 has registers, I/O, aperture
 * and a BAR of its own for the GTT; gen4 on share one BAR between the
 * registers and the GTT. */
#define GEN2_GMADR_BAR 0
#define GEN2_MMADR_BAR 1
#define GEN3_MMADR_BAR 0
#define GEN3_GMADR_BAR 2
#define GEN3_GTTADR_BAR 3
#define GEN4_GTTMMADR_BAR 0
#define GEN4_GMADR_BAR 2

#define MCHBAR_I915 0x44
#define MCHBAR_I965 0x48

/* host bridge (0:0.0), with a read-only mirror in the graphics function
 * from i85x/i865 on */
#define I830_GMCH_CTRL 0x52
#define I830_GMCH_ENABLED 0x4
#define I830_GMCH_MEM_MASK 0x1
#define I830_GMCH_MEM_64M 0x1
#define I830_GMCH_MEM_128M 0
#define I830_GMCH_GMS_MASK (0x7 << 4)
#define I830_GMCH_GMS_LOCAL (0x1 << 4)
#define I830_GMCH_GMS_STOLEN_512 (0x2 << 4)
#define I830_GMCH_GMS_STOLEN_1024 (0x3 << 4)
#define I830_GMCH_GMS_STOLEN_8192 (0x4 << 4)
#define I855_GMCH_GMS_MASK (0xF << 4)
#define I855_GMCH_GMS_STOLEN_0M (0x0 << 4)
#define I855_GMCH_GMS_STOLEN_1M (0x1 << 4)
#define I855_GMCH_GMS_STOLEN_4M (0x2 << 4)
#define I855_GMCH_GMS_STOLEN_8M (0x3 << 4)
#define I855_GMCH_GMS_STOLEN_16M (0x4 << 4)
#define I855_GMCH_GMS_STOLEN_32M (0x5 << 4)
#define I915_GMCH_GMS_STOLEN_48M (0x6 << 4)
#define I915_GMCH_GMS_STOLEN_64M (0x7 << 4)
#define G33_GMCH_GMS_STOLEN_128M (0x8 << 4)
#define G33_GMCH_GMS_STOLEN_256M (0x9 << 4)
#define INTEL_GMCH_GMS_STOLEN_96M (0xa << 4)
#define INTEL_GMCH_GMS_STOLEN_160M (0xb << 4)
#define INTEL_GMCH_GMS_STOLEN_224M (0xc << 4)
#define INTEL_GMCH_GMS_STOLEN_352M (0xd << 4)
#define G33_GMCH_SIZE_MASK (3 << 8)
#define G33_GMCH_SIZE_1M (1 << 8)
#define G33_GMCH_SIZE_2M (2 << 8)
#define G4X_GMCH_SIZE_MASK (0xf << 8)
#define G4X_GMCH_SIZE_1M (0x1 << 8)
#define G4X_GMCH_SIZE_2M (0x3 << 8)
#define G4X_GMCH_SIZE_VT_EN (0x8 << 8)
#define G4X_GMCH_SIZE_VT_1M (G4X_GMCH_SIZE_VT_EN | G4X_GMCH_SIZE_1M)
#define G4X_GMCH_SIZE_VT_1_5M (G4X_GMCH_SIZE_VT_EN | (0x2 << 8))
#define G4X_GMCH_SIZE_VT_2M (G4X_GMCH_SIZE_VT_EN | G4X_GMCH_SIZE_2M)

/* the graphics function itself (Gen6 on): stolen and GTT sizes */
#define SNB_GMCH_CTRL 0x50
#define SNB_GMCH_GGMS_SHIFT 8
#define SNB_GMCH_GGMS_MASK 0x3
#define SNB_GMCH_GMS_SHIFT 3
#define SNB_GMCH_GMS_MASK 0x1f

#define I830_DRB3 0x63
#define I85X_DRB3 0x43
#define I865_TOUD 0xc4
#define I830_ESMRAMC 0x91
#define I845_ESMRAMC 0x9e
#define I85X_ESMRAMC 0x61
#define TSEG_ENABLE (1 << 0)
#define I830_TSEG_SIZE_512K (0 << 1)
#define I830_TSEG_SIZE_1M (1 << 1)
#define I845_TSEG_SIZE_MASK (3 << 1)
#define I845_TSEG_SIZE_512K (2 << 1)
#define I845_TSEG_SIZE_1M (3 << 1)

#define INTEL_BSM 0x5c
#define INTEL_BSM_MASK 0xfff00000u

/* the chipset write-buffer flush page (gen3 and gen4) */
#define I915_IFPADDR 0x60
#define I965_IFPADDR 0x70

/* GPU reset through the graphics function's config space (gen3, gen4) */
#define I915_GDRST 0xc0
#define GRDOM_FULL (0 << 2)
#define GRDOM_RENDER (1 << 2)
#define GRDOM_MEDIA (3 << 2)
#define GRDOM_MASK (3 << 2)
#define GRDOM_RESET_STATUS (1 << 1)
#define GRDOM_RESET_ENABLE (1 << 0)

/* ---- the GTT ----------------------------------------------------------- */

#define I810_PTE_BASE 0x10000 /* gen2: the GTT inside the register BAR */
#define I810_PTE_VALID 0x00000001
#define I830_PTE_SYSTEM_CACHED 0x00000006
#define I810_PGETBL_CTL 0x2020
#define I810_PGETBL_ENABLED 0x00000001
#define I965_PGETBL_CTL2 0x20c4
#define I965_PGETBL_SIZE_MASK 0x0000000e
#define I965_PGETBL_SIZE_512KB (0 << 1)
#define I965_PGETBL_SIZE_256KB (1 << 1)
#define I965_PGETBL_SIZE_128KB (2 << 1)
#define I965_PGETBL_SIZE_1MB (3 << 1)
#define I965_PGETBL_SIZE_2MB (4 << 1)
#define I965_PGETBL_SIZE_1_5MB (5 << 1)
#define PGTBL_ER 0x02024
#define I830_HIC 0x70 /* gen2: host interface control, bit 31 flushes */
#define GFX_FLSH_CNTL 0x2170 /* gen3-5 */
#define GFX_FLSH_CNTL_GEN6 0x101008
#define GFX_FLSH_CNTL_EN (1 << 0)

/* Gen6/7 GGTT and PPGTT entries: 32 bits, the address's bits 39:32 in
 * entry bits 11:4 (Haswell 38:32 in 10:4). */
#define GEN6_PTE_VALID (1u << 0)
#define GEN6_PTE_UNCACHED (1u << 1)
#define GEN6_PTE_CACHE_LLC (2u << 1)
#define GEN7_PTE_CACHE_L3_LLC (3u << 1)
#define BYT_PTE_WRITEABLE (1u << 1)
#define BYT_PTE_SNOOPED_BY_CPU_CACHES (1u << 2)
#define HSW_CACHEABILITY_CONTROL(bits) ((((bits) & 0x7) << 1) | (((bits) & 0x8) << (11 - 3)))
#define HSW_WB_LLC_AGE3 HSW_CACHEABILITY_CONTROL(0x2)
#define HSW_WB_ELLC_LLC_AGE3 HSW_CACHEABILITY_CONTROL(0x8)
#define HSW_WT_ELLC_LLC_AGE3 HSW_CACHEABILITY_CONTROL(0x7)
#define GEN6_PDE_VALID (1u << 0)
#define GEN6_PTES 1024 /* per page table */
#define GEN6_PDES 512 /* per page directory: 2 GB */
#define GEN6_PDE_SHIFT 22
#define GEN6_PD_SIZE (GEN6_PDES * 4096u) /* the GGTT range the directory takes */
#define GEN6_PD_ALIGN (4096u * 16)

#define HSW_EDRAM_CAP 0x120010
#define EDRAM_ENABLED 0x1

/* ---- fence registers ------------------------------------------------------ */

/* [0-7] at 0x2000 on gen2/3, [8-15] at 0x3000 on 945, G33 and Pineview;
 * [0-15] at 0x3000 on gen4/5; at 0x100000 on gen6/7 (32 on IVB/HSW) */
#define FENCE_REG(i) (0x2000 + (((i) & 8) << 9) + ((i) & 7) * 4)
#define I830_FENCE_START_MASK 0x07f80000
#define I830_FENCE_TILING_Y_SHIFT 12
#define I830_FENCE_PITCH_SHIFT 4
#define I830_FENCE_REG_VALID (1u << 0)
#define I915_FENCE_START_MASK 0x0ff00000
#define FENCE_REG_965_LO(i) (0x03000 + (i) * 8)
#define FENCE_REG_965_HI(i) (0x03000 + (i) * 8 + 4)
#define I965_FENCE_PITCH_SHIFT 2
#define I965_FENCE_TILING_Y_SHIFT 1
#define I965_FENCE_REG_VALID (1u << 0)
#define I965_FENCE_MAX_PITCH_VAL 0x0400
#define I965_FENCE_PAGE 4096u
#define FENCE_REG_GEN6_LO(i) (0x100000 + (i) * 8)
#define FENCE_REG_GEN6_HI(i) (0x100000 + (i) * 8 + 4)
#define GEN6_FENCE_PITCH_SHIFT 32
#define GEN7_FENCE_MAX_PITCH_VAL 0x0800

#define TILECTL 0x101000
#define TILECTL_SWZCTL (1 << 0)
#define ARB_MODE 0x4030
#define ARB_MODE_SWIZZLE_SNB (1 << 4)
#define ARB_MODE_SWIZZLE_IVB (1 << 5)
#define DISP_ARB_CTL 0x45000
#define DISP_TILE_SURFACE_SWIZZLING (1 << 13)

/* the MCHBAR mirrors in the register window */
#define MCHBAR_MIRROR_BASE 0x10000
#define MCHBAR_MIRROR_BASE_SNB 0x140000
#define DCC (MCHBAR_MIRROR_BASE + 0x200)
#define DCC_ADDRESSING_MODE_SINGLE_CHANNEL (0 << 0)
#define DCC_ADDRESSING_MODE_DUAL_CHANNEL_ASYMMETRIC (1 << 0)
#define DCC_ADDRESSING_MODE_DUAL_CHANNEL_INTERLEAVED (2 << 0)
#define DCC_ADDRESSING_MODE_MASK (3 << 0)
#define DCC_CHANNEL_XOR_DISABLE (1 << 10)
#define DCC_CHANNEL_XOR_BIT_17 (1 << 9)
#define DCC2 (MCHBAR_MIRROR_BASE + 0x204)
#define DCC2_MODIFIED_ENHANCED_DISABLE (1 << 20)
#define C0DRB3_BW (MCHBAR_MIRROR_BASE + 0x206)
#define C1DRB3_BW (MCHBAR_MIRROR_BASE + 0x606)
#define CLKCFG (MCHBAR_MIRROR_BASE + 0xc00)
#define CLKCFG_FSB_400 (0 << 0)
#define CLKCFG_FSB_400_ALT (5 << 0)
#define CLKCFG_FSB_533 (1 << 0)
#define CLKCFG_FSB_667 (3 << 0)
#define CLKCFG_FSB_800 (2 << 0)
#define CLKCFG_FSB_1067 (6 << 0)
#define CLKCFG_FSB_1067_ALT (0 << 0)
#define CLKCFG_FSB_1333 (7 << 0)
#define CLKCFG_FSB_1333_ALT (4 << 0)
#define CLKCFG_FSB_1600_ALT (6 << 0)
#define CLKCFG_FSB_MASK (7 << 0)
#define ILK_GDSR (MCHBAR_MIRROR_BASE + 0x2ca4)
#define ILK_GRDOM_FULL (0 << 1)
#define ILK_GRDOM_RENDER (1 << 1)
#define ILK_GRDOM_MEDIA (3 << 1)
#define ILK_GRDOM_MASK (3 << 1)
#define ILK_GRDOM_RESET_ENABLE (1 << 0)
#define MAD_DIMM_C0 (MCHBAR_MIRROR_BASE_SNB + 0x5004)
#define MAD_DIMM_C1 (MCHBAR_MIRROR_BASE_SNB + 0x5008)
#define MAD_DIMM_B_SIZE_SHIFT 8
#define MAD_DIMM_A_SIZE_SHIFT 0
#define MAD_DIMM_B_SIZE_MASK (0xff << MAD_DIMM_B_SIZE_SHIFT)
#define MAD_DIMM_A_SIZE_MASK (0xff << MAD_DIMM_A_SIZE_SHIFT)

/* ---- engines -------------------------------------------------------------- */

#define RENDER_RING_BASE 0x02000
#define BSD_RING_BASE 0x04000 /* G4x, Ironlake */
#define GEN6_BSD_RING_BASE 0x12000
#define VEBOX_RING_BASE 0x1a000
#define BLT_RING_BASE 0x22000
/* the rings gen2/3 have and the driver leaves idle */
#define PRB1_BASE (0x2040 - 0x30)
#define PRB2_BASE (0x2050 - 0x30)
#define SRB0_BASE (0x2100 - 0x30)
#define SRB1_BASE (0x2110 - 0x30)
#define SRB2_BASE (0x2120 - 0x30)
#define SRB3_BASE (0x2130 - 0x30)

#define RING_TAIL(base) ((base) + 0x30)
#define TAIL_ADDR 0x001FFFF8
#define RING_HEAD(base) ((base) + 0x34)
#define HEAD_ADDR 0x001FFFFC
#define RING_START(base) ((base) + 0x38)
#define RING_CTL(base) ((base) + 0x3c)
#define RING_CTL_SIZE(size) ((size) - 4096)
#define RING_VALID 0x00000001
#define RING_WAIT (1 << 11)
#define RING_PSMI_CTL(base) ((base) + 0x50)
#define GEN6_BSD_GO_INDICATOR (1 << 4)
#define GEN6_BSD_SLEEP_INDICATOR (1 << 3)
#define GEN6_BSD_SLEEP_FLUSH_DISABLE (1 << 2)
#define GEN6_PSMI_SLEEP_MSG_DISABLE (1 << 0)
#define RING_MAX_IDLE(base) ((base) + 0x54)
#define RING_IPEIR(base) ((base) + 0x64)
#define RING_IPEHR(base) ((base) + 0x68)
#define RING_INSTDONE(base) ((base) + 0x6c)
#define RING_INSTPS(base) ((base) + 0x70)
#define RING_ACTHD(base) ((base) + 0x74) /* gen4+ */
#define RING_HWS_PGA(base) ((base) + 0x80)
#define RING_HWSTAM(base) ((base) + 0x98)
#define RING_MI_MODE(base) ((base) + 0x9c)
#define ASYNC_FLIP_PERF_DISABLE (1 << 14)
#define MODE_IDLE (1 << 9)
#define STOP_RING (1 << 8)
#define VS_TIMER_DISPATCH (1 << 6)
#define RING_IMR(base) ((base) + 0xa8)
#define RING_INSTPM(base) ((base) + 0xc0)
#define ACTHD_I830(base) ((base) + 0xc8) /* gen2/3 */
#define RING_BBADDR(base) ((base) + 0x140)
#define ECOSKPD(base) ((base) + 0x1d0)
#define ECO_CONSTANT_BUFFER_SR_DISABLE (1 << 4)
#define ECO_GATING_CX_ONLY (1 << 3)
#define ECO_FLIP_DONE (1 << 0)
#define RING_PP_DIR_DCLV(base) ((base) + 0x220)
#define PP_DIR_DCLV_2G 0xffffffff
#define RING_PP_DIR_BASE(base) ((base) + 0x228)
#define RING_MODE_GEN7(base) ((base) + 0x29c)
#define GFX_TLB_INVALIDATE_EXPLICIT (1 << 13)
#define GFX_REPLAY_MODE (1 << 11)
#define GFX_PPGTT_ENABLE (1 << 9)
#define RING_HWS_PGA_GEN6(base) ((base) + 0x2080)

#define HWS_PGA 0x2080
#define RENDER_HWS_PGA_GEN7 0x4080
#define BSD_HWS_PGA_GEN7 0x4180
#define BLT_HWS_PGA_GEN7 0x4280
#define VEBOX_HWS_PGA_GEN7 0x4380

#define INSTPM 0x20c0
#define INSTPM_AGPBUSY_INT_EN (1 << 11)
#define INSTPM_TLB_INVALIDATE (1 << 9)
#define INSTPM_FORCE_ORDERING (1 << 7)
#define INSTPM_SYNC_FLUSH (1 << 5)
#define GFX_MODE 0x2520 /* gen6 */
#define CXT_SIZE 0x21a0
#define GEN6_CXT_RING_SIZE(r) (((r) >> 18) & 0x3f)
#define GEN6_CXT_EXTENDED_SIZE(r) (((r) >> 6) & 0x3f)
#define GEN6_CXT_PIPELINE_SIZE(r) (((r) >> 0) & 0x3f)
#define GEN6_CXT_TOTAL_SIZE(r) \
	(GEN6_CXT_RING_SIZE(r) + GEN6_CXT_EXTENDED_SIZE(r) + GEN6_CXT_PIPELINE_SIZE(r))
#define GEN7_CXT_SIZE 0x21a8
#define GEN7_CXT_EXTENDED_SIZE(r) (((r) >> 9) & 0x7f)
#define GEN7_CXT_VFSTATE_SIZE(r) (((r) >> 0) & 0x3f)
#define GEN7_CXT_TOTAL_SIZE(r) (GEN7_CXT_EXTENDED_SIZE(r) + GEN7_CXT_VFSTATE_SIZE(r))
#define HSW_CXT_TOTAL_SIZE (17 * 4096)
#define HSW_MI_PREDICATE_RESULT_2 0x2214
#define LOWER_SLICE_ENABLED (1 << 0)
#define LOWER_SLICE_DISABLED (0 << 0)
#define GEN6_BSD_RNCID 0x12198
#define GEN7_FF_THREAD_MODE 0x20a0
#define GEN7_FF_SCHED_MASK 0x0077070
#define GEN7_FF_TS_SCHED_HW (0x0 << 16)
#define GEN7_FF_VS_REF_CNT_FFME (1 << 15)
#define GEN7_FF_VS_SCHED_HW (0x0 << 12)
#define GEN7_FF_DS_SCHED_HW (0x0 << 4)

#define GEN6_GDRST 0x941c
#define GEN6_GRDOM_FULL (1 << 0)
#define GEN6_GRDOM_RENDER (1 << 1)
#define GEN6_GRDOM_MEDIA (1 << 2)
#define GEN6_GRDOM_BLT (1 << 3)
#define GEN6_GRDOM_VECS (1 << 4)

/* ---- command streamer instructions ------------------------------------- */

#define MI_INSTR(opcode, flags) (((opcode) << 23) | (flags))
#define MI_NOOP MI_INSTR(0, 0)
#define MI_USER_INTERRUPT MI_INSTR(0x02, 0)
#define MI_FLUSH MI_INSTR(0x04, 0)
#define MI_READ_FLUSH (1 << 0)
#define MI_EXE_FLUSH (1 << 1)
#define MI_NO_WRITE_FLUSH (1 << 2)
#define MI_INVALIDATE_ISP (1 << 5)
#define MI_ARB_ON_OFF MI_INSTR(0x08, 0)
#define MI_ARB_ENABLE (1 << 0)
#define MI_ARB_DISABLE (0 << 0)
#define MI_BATCH_BUFFER_END MI_INSTR(0x0a, 0)
#define MI_SUSPEND_FLUSH MI_INSTR(0x0b, 0)
#define MI_SUSPEND_FLUSH_EN (1 << 0)
#define MI_SET_CONTEXT MI_INSTR(0x18, 0)
#define MI_MM_SPACE_GTT (1 << 8)
#define MI_SAVE_EXT_STATE_EN (1 << 3)
#define MI_RESTORE_EXT_STATE_EN (1 << 2)
#define MI_FORCE_RESTORE (1 << 1)
#define MI_RESTORE_INHIBIT (1 << 0)
#define MI_STORE_DWORD_INDEX MI_INSTR(0x21, 1)
#define MI_LOAD_REGISTER_IMM(x) MI_INSTR(0x22, 2 * (x) - 1)
#define MI_STORE_REGISTER_MEM MI_INSTR(0x24, 1)
#define MI_SRM_LRM_GLOBAL_GTT (1 << 22)
#define MI_FLUSH_DW MI_INSTR(0x26, 1)
#define MI_FLUSH_DW_STORE_INDEX (1 << 21)
#define MI_INVALIDATE_TLB (1 << 18)
#define MI_FLUSH_DW_OP_STOREDW (1 << 14)
#define MI_INVALIDATE_BSD (1 << 7)
#define MI_FLUSH_DW_USE_GTT (1 << 2)
#define MI_BATCH_NON_SECURE (1)
#define MI_BATCH_NON_SECURE_I965 (1 << 8) /* SNB/IVB/VLV: in the PPGTT */
#define MI_BATCH_PPGTT_HSW (1 << 8)
#define MI_BATCH_NON_SECURE_HSW (1 << 13)
#define MI_BATCH_BUFFER_START MI_INSTR(0x31, 0)
#define MI_BATCH_GTT (2 << 6)

#define GFX_OP_PIPE_CONTROL(len) ((0x3 << 29) | (0x3 << 27) | (0x2 << 24) | ((len) - 2))
#define PIPE_CONTROL_GLOBAL_GTT_IVB (1 << 24)
#define PIPE_CONTROL_CS_STALL (1 << 20)
#define PIPE_CONTROL_TLB_INVALIDATE (1 << 18)
#define PIPE_CONTROL_MEDIA_STATE_CLEAR (1 << 16)
#define PIPE_CONTROL_QW_WRITE (1 << 14)
#define PIPE_CONTROL_DEPTH_STALL (1 << 13)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH (1 << 12)
#define PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE (1 << 11)
#define PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE (1 << 10)
#define PIPE_CONTROL_FLUSH_ENABLE (1 << 7)
#define PIPE_CONTROL_DC_FLUSH_ENABLE (1 << 5)
#define PIPE_CONTROL_VF_CACHE_INVALIDATE (1 << 4)
#define PIPE_CONTROL_CONST_CACHE_INVALIDATE (1 << 3)
#define PIPE_CONTROL_STATE_CACHE_INVALIDATE (1 << 2)
#define PIPE_CONTROL_STALL_AT_SCOREBOARD (1 << 1)
#define PIPE_CONTROL_DEPTH_CACHE_FLUSH (1 << 0)
#define PIPE_CONTROL_GLOBAL_GTT (1 << 2) /* in the address dword */

#define COLOR_BLT_CMD (2 << 29 | 0x40 << 22 | (5 - 2))
#define SRC_COPY_BLT_CMD (2 << 29 | 0x43 << 22)
#define BLT_WRITE_A (2 << 20)
#define BLT_WRITE_RGB (1 << 20)
#define BLT_WRITE_RGBA (BLT_WRITE_RGB | BLT_WRITE_A)
#define BLT_DEPTH_32 (3 << 24)
#define BLT_ROP_SRC_COPY (0xcc << 16)
#define BLT_ROP_COLOR_COPY (0xf0 << 16)

/* ---- interrupts ------------------------------------------------------------ */

/* gen2-4 (and Valleyview's display, at VLV_DISPLAY_BASE) */
#define GEN2_IER 0x20a0
#define GEN2_IIR 0x20a4
#define GEN2_IMR 0x20a8
#define GEN2_ISR 0x20ac
#define EIR 0x20b0
#define EMR 0x20b4
#define ESR 0x20b8
#define GM45_ERROR_PAGE_TABLE (1 << 5)
#define GM45_ERROR_MEM_PRIV (1 << 4)
#define I915_ERROR_PAGE_TABLE (1 << 4)
#define GM45_ERROR_CP_PRIV (1 << 3)
#define I915_ERROR_MEMORY_REFRESH (1 << 1)
#define I915_ERROR_INSTRUCTION (1 << 0)
#define I915_BSD_USER_INTERRUPT (1 << 25)
#define I915_DISPLAY_PORT_INTERRUPT (1 << 17)
#define I915_MASTER_ERROR_INTERRUPT (1 << 15)
#define I915_DISPLAY_PIPE_A_EVENT_INTERRUPT (1 << 6)
#define I915_DISPLAY_PIPE_B_EVENT_INTERRUPT (1 << 4)
#define I915_USER_INTERRUPT (1 << 1)
#define I915_ASLE_INTERRUPT (1 << 0)

#define VLV_DISPLAY_BASE 0x180000
#define VLV_IER (VLV_DISPLAY_BASE + 0x20a0)
#define VLV_IIR (VLV_DISPLAY_BASE + 0x20a4)
#define VLV_IMR (VLV_DISPLAY_BASE + 0x20a8)
#define VLV_ISR (VLV_DISPLAY_BASE + 0x20ac)
#define VLV_MASTER_IER 0x4400c /* Gunit master IER */
#define MASTER_INTERRUPT_ENABLE (1u << 31)
#define CLAIM_ER (VLV_DISPLAY_BASE + 0x2028)
#define CLAIM_ER_CLR (1u << 31)
#define CLAIM_ER_OVERFLOW (1 << 16)
#define CLAIM_ER_CTR_MASK 0xffff
#define VLV_GUNIT_CLOCK_GATE (VLV_DISPLAY_BASE + 0x2060)
#define GCFG_DIS (1 << 8)
#define VLV_PCBR (VLV_DISPLAY_BASE + 0x2120)

/* Ironlake to Haswell: the north display's interrupts hold the master
 * enable; the GT and the power management unit have their own. */
#define DEISR 0x44000
#define DEIMR 0x44004
#define DEIIR 0x44008
#define DEIER 0x4400c
#define DE_MASTER_IRQ_CONTROL (1u << 31)
#define DE_ERR_INT_IVB (1 << 30)
#define DE_GSE_IVB (1 << 29)
#define DE_PCH_EVENT_IVB (1 << 28)
#define DE_DP_A_HOTPLUG_IVB (1 << 27)
#define DE_AUX_CHANNEL_A_IVB (1 << 26)
#define DE_PIPE_VBLANK_IVB(pipe) (1 << ((pipe) * 5))
#define DE_PLANE_FLIP_DONE_IVB(plane) (1 << (3 + 5 * (plane)))
#define GEN7_ERR_INT 0x44040
#define ERR_INT_FIFO_UNDERRUN(pipe) (1 << ((pipe) * 3))
#define GTISR 0x44010
#define GTIMR 0x44014
#define GTIIR 0x44018
#define GTIER 0x4401c
#define GEN6_PMISR 0x44020
#define GEN6_PMIMR 0x44024
#define GEN6_PMIIR 0x44028
#define GEN6_PMIER 0x4402c
#define SDEIMR 0xc4004
#define SDEIIR 0xc4008
#define SDEIER 0xc400c

#define GT_BLT_CS_ERROR_INTERRUPT (1 << 25)
#define GT_BLT_USER_INTERRUPT (1 << 22)
#define GT_BSD_CS_ERROR_INTERRUPT (1 << 15)
#define GT_BSD_USER_INTERRUPT (1 << 12)
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT_S1 (1 << 11) /* hsw */
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT (1 << 5)
#define GT_CS_MASTER_ERROR_INTERRUPT (1 << 3)
#define GT_RENDER_USER_INTERRUPT (1 << 0)
#define ILK_BSD_USER_INTERRUPT (1 << 5)
#define PM_VEBOX_CS_ERROR_INTERRUPT (1 << 12)
#define PM_VEBOX_USER_INTERRUPT (1 << 10)
#define GEN6_PM_RP_DOWN_TIMEOUT (1 << 6)
#define GEN6_PM_RP_UP_THRESHOLD (1 << 5)
#define GEN6_PM_RP_DOWN_THRESHOLD (1 << 4)
#define GEN6_PM_RP_UP_EI_EXPIRED (1 << 2)
#define GEN6_PM_RP_DOWN_EI_EXPIRED (1 << 1)
#define GEN6_PM_RPS_EVENTS                                                     \
	(GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_UP_THRESHOLD |                  \
	 GEN6_PM_RP_DOWN_EI_EXPIRED | GEN6_PM_RP_DOWN_THRESHOLD |              \
	 GEN6_PM_RP_DOWN_TIMEOUT)

/* Cherryview: the Gen8 GT interrupt banks under the Gen8 master */
#define GEN8_MASTER_IRQ 0x44200
#define GEN8_MASTER_IRQ_CONTROL (1u << 31)
#define GEN8_PCU_IRQ (1 << 30)
#define GEN8_GT_VECS_IRQ (1 << 6)
#define GEN8_GT_GUC_IRQ (1 << 5)
#define GEN8_GT_PM_IRQ (1 << 4)
#define GEN8_GT_VCS1_IRQ (1 << 3)
#define GEN8_GT_VCS0_IRQ (1 << 2)
#define GEN8_GT_BCS_IRQ (1 << 1)
#define GEN8_GT_RCS_IRQ (1 << 0)
#define GEN8_GT_IMR(n) (0x44304 + 0x10 * (n))
#define GEN8_GT_IIR(n) (0x44308 + 0x10 * (n))
#define GEN8_GT_IER(n) (0x4430c + 0x10 * (n))
#define GEN8_PCU_IMR 0x444e4
#define GEN8_PCU_IIR 0x444e8
#define GEN8_PCU_IER 0x444ec
#define GEN8_RCS_IRQ_SHIFT 0
#define GEN8_BCS_IRQ_SHIFT 16
#define GEN8_VCS0_IRQ_SHIFT 0
#define GEN8_VCS1_IRQ_SHIFT 16
#define GEN8_VECS_IRQ_SHIFT 0
#define GT_CONTEXT_SWITCH_INTERRUPT (1 << 8)
#define GT_WAIT_SEMAPHORE_INTERRUPT (1 << 11)

/* the bits the north display's handlers of the Gen8 layout understand */
#define GEN8_PIPE_VBLANK (1 << 0)
#define GEN8_PIPE_PRIMARY_FLIP_DONE (1 << 4)
#define GEN8_PIPE_FIFO_UNDERRUN (1u << 31)
#define GEN8_AUX_CHANNEL_A (1 << 0)
#define GEN8_PORT_DP_A_HOTPLUG (1 << 3)

/* ---- forcewake and the GT FIFO (Gen6/7) ----------------------------------- */

#define FORCEWAKE 0xa18c
#define FORCEWAKE_MT 0xa188
#define ECOBUS 0xa180
#define FORCEWAKE_MT_ENABLE (1 << 5)
#define FORCEWAKE_KERNEL (1 << 0)
#define FORCEWAKE_ACK 0x130090
#define FORCEWAKE_MT_ACK 0x130040
#define FORCEWAKE_ACK_HSW 0x130044
#define FORCEWAKE_VLV 0x1300b0
#define FORCEWAKE_ACK_VLV 0x1300b4
#define FORCEWAKE_MEDIA_VLV 0x1300b8
#define FORCEWAKE_ACK_MEDIA_VLV 0x1300bc
#define GTFIFODBG 0x120000
#define GTFIFOCTL 0x120008
#define GT_FIFO_FREE_ENTRIES_MASK 0x7f
#define GT_FIFO_NUM_RESERVED_ENTRIES 20
#define GEN6_GT_THREAD_STATUS_REG 0x13805c
#define GEN6_GT_THREAD_STATUS_CORE_MASK 0x7
#define FPGA_DBG 0x42300
#define FPGA_DBG_RM_NOCLAIM (1u << 31)

/* the PCU mailbox (Gen6 on) */
#define GEN6_PCODE_MAILBOX 0x138124
#define GEN6_PCODE_READY (1u << 31)
#define GEN6_PCODE_ERROR_MASK 0xff
#define GEN6_PCODE_SUCCESS 0x0
#define GEN6_PCODE_ILLEGAL_CMD 0x1
#define GEN6_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE 0x2
#define GEN6_PCODE_TIMEOUT 0x3
#define GEN6_PCODE_UNIMPLEMENTED_CMD 0xff
#define GEN7_PCODE_TIMEOUT 0x2
#define GEN7_PCODE_ILLEGAL_DATA 0x3
#define GEN7_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE 0x10
#define GEN6_PCODE_WRITE_RC6VIDS 0x4
#define GEN6_PCODE_READ_RC6VIDS 0x5
#define GEN6_ENCODE_RC6_VID(mv) (((mv) - 245) / 5)
#define GEN6_DECODE_RC6_VID(vids) (((vids) * 5) + 245)
#define GEN6_READ_OC_PARAMS 0xc
#define HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL 0x1a
#define GEN6_PCODE_DATA 0x138128
#define GEN6_PCODE_DATA1 0x13812c

/* the IOSF sideband (Valleyview) */
#define VLV_IOSF_DOORBELL_REQ (VLV_DISPLAY_BASE + 0x2100)
#define IOSF_DEVFN_SHIFT 24
#define IOSF_OPCODE_SHIFT 16
#define IOSF_PORT_SHIFT 8
#define IOSF_BYTE_ENABLES_SHIFT 4
#define IOSF_BAR_SHIFT 1
#define IOSF_SB_BUSY (1 << 0)
#define IOSF_PORT_PUNIT 0x04
#define IOSF_PORT_NC 0x11
#define VLV_IOSF_DATA (VLV_DISPLAY_BASE + 0x2104)
#define VLV_IOSF_ADDR (VLV_DISPLAY_BASE + 0x2108)
#define SB_MRD_NP 0x00
#define SB_MWR_NP 0x01
#define SB_CRRDDA_NP 0x06
#define SB_CRWRDA_NP 0x07
#define PUNIT_REG_GPU_LFM 0xd3
#define PUNIT_REG_GPU_FREQ_REQ 0xd4
#define PUNIT_REG_GPU_FREQ_STS 0xd8
#define GPLLENABLE (1 << 4)
#define IOSF_NC_FB_GFX_FREQ_FUSE 0x1c
#define FB_GFX_MAX_FREQ_FUSE_SHIFT 3
#define FB_GFX_MAX_FREQ_FUSE_MASK 0x000007f8
#define FB_GFX_FGUARANTEED_FREQ_FUSE_SHIFT 11
#define FB_GFX_FGUARANTEED_FREQ_FUSE_MASK 0x0007f800
#define IOSF_NC_FB_GFX_FMAX_FUSE_HI 0x34
#define FB_FMAX_VMIN_FREQ_HI_MASK 0x00000007
#define IOSF_NC_FB_GFX_FMAX_FUSE_LO 0x30
#define FB_FMAX_VMIN_FREQ_LO_SHIFT 27
#define FB_FMAX_VMIN_FREQ_LO_MASK 0xf8000000
#define VLV_TURBO_SOC_OVERRIDE 0x04
#define VLV_OVERRIDE_EN 1
#define VLV_SOC_TDP_EN (1 << 1)
#define VLV_BIAS_CPU_125_SOC_875 (6 << 2)

/* ---- render P-states and render standby (Gen6/7) ---------------------- */

#define GEN6_RPNSWREQ 0xa008
#define GEN6_FREQUENCY(x) ((x) << 25)
#define HSW_FREQUENCY(x) ((x) << 24)
#define GEN6_OFFSET(x) ((x) << 19)
#define GEN6_AGGRESSIVE_TURBO (0 << 15)
#define GEN6_RC_VIDEO_FREQ 0xa00c
#define GEN6_RP_DOWN_TIMEOUT 0xa010
#define GEN6_RP_INTERRUPT_LIMITS 0xa014
#define GEN6_RPSTAT1 0xa01c
#define GEN6_CAGF_SHIFT 8
#define GEN6_CAGF_MASK (0x7f << 8)
#define HSW_CAGF_SHIFT 7
#define HSW_CAGF_MASK (0x7f << 7)
#define GEN6_RP_CONTROL 0xa024
#define GEN6_RP_MEDIA_TURBO (1 << 11)
#define GEN6_RP_MEDIA_HW_NORMAL_MODE (2 << 9)
#define GEN6_RP_MEDIA_IS_GFX (1 << 8)
#define GEN6_RP_ENABLE (1 << 7)
#define GEN6_RP_UP_BUSY_AVG (0x2 << 3)
#define GEN6_RP_DOWN_IDLE_AVG (0x2 << 0)
#define GEN6_RP_DOWN_IDLE_CONT (0x1 << 0)
#define GEN6_RP_UP_THRESHOLD 0xa02c
#define GEN6_RP_DOWN_THRESHOLD 0xa030
#define GEN6_RP_UP_EI 0xa068
#define GEN6_RP_DOWN_EI 0xa06c
#define GEN6_RP_IDLE_HYSTERSIS 0xa070
#define GEN6_RP_STATE_CAP (MCHBAR_MIRROR_BASE_SNB + 0x5998)
#define GEN6_RC_CONTROL 0xa090
#define GEN6_RC_CTL_RC6pp_ENABLE (1 << 16)
#define GEN6_RC_CTL_RC6p_ENABLE (1 << 17)
#define GEN6_RC_CTL_RC6_ENABLE (1 << 18)
#define VLV_RC_CTL_CTX_RST_PARALLEL (1 << 24)
#define GEN6_RC_CTL_EI_MODE(x) ((x) << 27)
#define GEN7_RC_CTL_TO_MODE (1 << 28)
#define GEN6_RC_CTL_HW_ENABLE (1u << 31)
#define GEN6_RC_STATE 0xa094
#define GEN6_RC1_WAKE_RATE_LIMIT 0xa098
#define GEN6_RC6_WAKE_RATE_LIMIT 0xa09c
#define GEN6_RC6pp_WAKE_RATE_LIMIT 0xa0a0
#define GEN6_RC_EVALUATION_INTERVAL 0xa0a8
#define GEN6_RC_IDLE_HYSTERSIS 0xa0ac
#define GEN6_RC_SLEEP 0xa0b0
#define GEN6_RC1e_THRESHOLD 0xa0b4
#define GEN6_RC6_THRESHOLD 0xa0b8
#define GEN6_RC6p_THRESHOLD 0xa0bc
#define GEN6_RC6pp_THRESHOLD 0xa0c0
#define GEN6_PMINTRMSK 0xa168
#define VLV_COUNTER_CONTROL 0x138104
#define VLV_COUNT_RANGE_HIGH (1 << 15)
#define VLV_MEDIA_RC0_COUNT_EN (1 << 5)
#define VLV_RENDER_RC0_COUNT_EN (1 << 4)
#define VLV_MEDIA_RC6_COUNT_EN (1 << 1)
#define VLV_RENDER_RC6_COUNT_EN (1 << 0)

/* ---- workarounds and clock gating ----------------------------------------- */

#define _3D_CHICKEN 0x2084
#define _3D_CHICKEN_HIZ_PLANE_DISABLE_MSAA_4X_SNB (1 << 10)
#define _3D_CHICKEN2 0x208c
#define _3D_CHICKEN2_WM_READ_PIPELINED (1 << 14)
#define _3D_CHICKEN3 0x2090
#define _3D_CHICKEN_SF_DISABLE_OBJEND_CULL (1 << 10)
#define _3D_CHICKEN3_SF_DISABLE_FASTCLIP_CULL (1 << 5)
#define _3D_CHICKEN3_SF_DISABLE_PIPELINED_ATTR_FETCH (1 << 1)
#define GEN6_GT_MODE 0x20d0
#define GEN6_WIZ_HASHING(hi, lo) (((hi) << 9) | ((lo) << 7))
#define GEN6_WIZ_HASHING_16x4 GEN6_WIZ_HASHING(1, 0)
#define GEN6_WIZ_HASHING_MASK GEN6_WIZ_HASHING(1, 1)
#define CACHE_MODE_0 0x2120
#define CM0_PIPELINED_RENDER_FLUSH_DISABLE (1 << 8)
#define CM0_STC_EVICT_DISABLE_LRA_SNB (1 << 5)
#define RC_OP_FLUSH_ENABLE (1 << 0)
#define CACHE_MODE_0_GEN7 0x7000
#define HIZ_RAW_STALL_OPT_DISABLE (1 << 2)
#define CACHE_MODE_1 0x7004
#define PIXEL_SUBSPAN_COLLECT_OPT_DISABLE (1 << 6)
#define GEN7_GT_MODE 0x7008
#define GEN7_COMMON_SLICE_CHICKEN1 0x7010
#define GEN7_CSC1_RHWO_OPT_DISABLE_IN_RCC (1 << 10)
#define GEN7_L3SQCREG1 0xb010
#define VLV_B0_WA_L3SQCREG1_VALUE 0x00D30000
#define GEN7_L3CNTLREG1 0xb01c
#define GEN7_WA_FOR_GEN7_L3_CONTROL 0x3C47FF8C
#define GEN7_L3_CHICKEN_MODE_REGISTER 0xb030
#define GEN7_WA_L3_CHICKEN_MODE 0x20000000
#define GEN7_L3SQCREG4 0xb034
#define L3SQ_URB_READ_CAM_MATCH_DISABLE (1 << 27)
#define HSW_SCRATCH1 0xb038
#define HSW_SCRATCH1_L3_DATA_ATOMICS_DISABLE (1 << 27)
#define GEN7_HALF_SLICE_CHICKEN1 0xe100
#define GEN7_MAX_PS_THREAD_DEP (8 << 12)
#define GEN7_PSD_SINGLE_PORT_DISPATCH_ENABLE (1 << 3)
#define HSW_HALF_SLICE_CHICKEN3 0xe184
#define HSW_SAMPLE_C_PERFORMANCE (1 << 9)
#define HSW_ROW_CHICKEN3 0xe49c
#define HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE (1 << 6)
#define GEN7_ROW_CHICKEN2 0xe4f4
#define GEN7_ROW_CHICKEN2_GT2 0xf4f4
#define DOP_CLOCK_GATING_DISABLE (1 << 0)
#define GEN6_MBCUNIT_SNPCR 0x900c
#define GEN6_MBC_SNPCR_MASK (3 << 21)
#define GEN6_MBC_SNPCR_MED (1 << 21)
#define GEN7_SQ_CHICKEN_MBCUNIT_CONFIG 0x9030
#define GEN7_SQ_CHICKEN_MBCUNIT_SQINTMOB (1 << 11)
#define HSW_IDICR 0x9008
#define IDIHASHMSK(x) (((x) & 0x3f) << 16)
#define GEN6_UCGCTL1 0x9400
#define GEN6_BLBUNIT_CLOCK_GATE_DISABLE (1 << 5)
#define GEN6_CSUNIT_CLOCK_GATE_DISABLE (1 << 7)
#define GEN6_UCGCTL2 0x9404
#define GEN6_RCZUNIT_CLOCK_GATE_DISABLE (1 << 13)
#define GEN6_RCPBUNIT_CLOCK_GATE_DISABLE (1 << 12)
#define GEN6_RCCUNIT_CLOCK_GATE_DISABLE (1 << 11)
#define GEN7_UCGCTL4 0x940c
#define GEN7_L3BANK2X_CLOCK_GATE_DISABLE (1 << 25)
#define GAM_ECOCHK 0x4090
#define ECOCHK_SNB_BIT (1 << 10)
#define HSW_ECOCHK_ARB_PRIO_SOL (1 << 6)
#define ECOCHK_PPGTT_CACHE64B (0x3 << 3)
#define ECOCHK_PPGTT_GFDT_IVB (0x1 << 4)
#define ECOCHK_PPGTT_LLC_IVB (0x1 << 3)
#define ECOCHK_PPGTT_WB_HSW (0x3 << 3)
#define GAC_ECO_BITS 0x14090
#define ECOBITS_SNB_BIT (1 << 13)
#define ECOBITS_PPGTT_CACHE64B (3 << 8)
#define GAB_CTL 0x24000
#define GAB_CTL_CONT_AFTER_PAGEFAULT (1 << 8)
#define IVB_CHICKEN3 0x4200c
#define CHICKEN3_DGMG_REQ_OUT_FIX_DISABLE (1 << 5)
#define CHICKEN3_DGMG_DONE_FIX_DISABLE (1 << 2)
#define PCH_3DCGDIS0 0x46020
#define MARIUNIT_CLOCK_GATE_DISABLE (1 << 18)
#define SVSMUNIT_CLOCK_GATE_DISABLE (1 << 1)
#define PCH_3DCGDIS1 0x46024
#define VFMUNIT_CLOCK_GATE_DISABLE (1 << 11)
#define D_STATE 0x6104
#define DSTATE_PLL_D3_OFF (1 << 3)
#define DSTATE_GFX_CLOCK_GATING (1 << 1)
#define DSTATE_DOT_CLOCK_GATING (1 << 0)
#define RENCLK_GATE_D1 0x6204
#define SV_CLOCK_GATE_DISABLE (1 << 0)
#define I965_RCZ_CLOCK_GATE_DISABLE (1 << 30)
#define I965_RCC_CLOCK_GATE_DISABLE (1 << 29)
#define I965_RCPB_CLOCK_GATE_DISABLE (1 << 28)
#define I965_ISC_CLOCK_GATE_DISABLE (1 << 23)
#define I965_FBC_CLOCK_GATE_DISABLE (1 << 16)
#define RENCLK_GATE_D2 0x6208
#define VF_UNIT_CLOCK_GATE_DISABLE (1 << 9)
#define GS_UNIT_CLOCK_GATE_DISABLE (1 << 7)
#define CL_UNIT_CLOCK_GATE_DISABLE (1 << 6)
#define VDECCLK_GATE_D 0x620c
#define VCP_UNIT_CLOCK_GATE_DISABLE (1 << 4)
#define RAMCLK_GATE_D 0x6210
#define DEUC 0x6214
#define MEM_MODE 0x20cc
#define MEM_DISPLAY_B_TRICKLE_FEED_DISABLE (1 << 3)
#define MEM_DISPLAY_A_TRICKLE_FEED_DISABLE (1 << 2)
#define MEM_DISPLAY_TRICKLE_FEED_DISABLE (1 << 2)
#define MI_ARB_STATE 0x20e4
#define MI_ARB_C3_LP_WRITE_ENABLE (1 << 11)
#define MI_ARB_DISPLAY_TRICKLE_FEED_DISABLE (1 << 2)
#define MI_STATE 0x20e4 /* gen2 */
#define MI_AGPBUSY_INT_EN (1 << 1)
#define MI_AGPBUSY_830_MODE (1 << 0)
#define SCPD0 0x209c
#define SCPD_FBC_IGNORE_3D (1 << 6)

/* ---- the display registers the GT side reads ----------------------------- */

#define _PIPEACONF 0x70008
#define PIPECONF_ENABLE (1u << 31)
#define _PIPEASRC 0x6001c
#define _DSPACNTR 0x70180
#define DISPLAY_PLANE_ENABLE (1u << 31)
#define _DSPAADDR 0x70184 /* gen2/3: the surface's address */
#define _DSPASTRIDE 0x70188
#define _DSPASURF 0x7019c /* gen4 on */
#define LEG_PIPE_REG(base, pipe) ((base) + (pipe) * 0x1000)

#endif
