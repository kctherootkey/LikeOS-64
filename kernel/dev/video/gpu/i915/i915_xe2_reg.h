// LikeOS -- the GT registers of Xe2 (Lunar Lake, Battlemage), Xe3 (Panther
// Lake, Wildcat Lake, Nova Lake-S) and Xe3P (Nova Lake-P) graphics, and of
// their standalone media GTs.
//
// Only what the driver programs or reads on these parts and does not find
// in i915_reg.h, i915_wa_reg.h or i915_guc_reg.h already.  Offsets of the
// media GT's registers below 0x40000 are given as the primary GT's; the
// code adds I915_MEDIA_GT_BASE (i915_gt_reg()).  A register with one copy
// per slice, subslice, bank or node is marked MCR: it is read through the
// steering and written to every copy (i915_mcr.c).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2022-2025 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_XE2_REG_H
#define KERNEL_DEV_GPU_I915_XE2_REG_H

/* ---- the IP versions ------------------------------------------------------ */

/* GMD_ID: bits 13:6 name the sub-IP of an Xe3P media GT whose engines
 * and GuC are still Xe3's (0 = all Xe3P). */
#define GMD_ID_SUBIP_FLAG(v) (((v) >> 6) & 0xff)

/* ---- topology ---------------------------------------------------------------- */

#define XE2_GT_COMPUTE_DSS_EXT 0x9148 /* compute DSS 32-63 */
#define XE2_GT_COMPUTE_DSS_2 0x914c /* compute DSS 64-95 */
#define XE2_GT_GEOMETRY_DSS_1 0x9150 /* geometry DSS 32-63 */
#define XE2_GT_GEOMETRY_DSS_2 0x9154 /* geometry DSS 64-95 */
/* MIRROR_FUSE3 (GEN10_MIRROR_FUSE3): the L3 nodes, and the banks each
 * node has */
#define XE2_NODE_ENABLE(v) (((v) >> 16) & 0xffff)
#define XE2_GT_L3_MODE(v) (((v) >> 4) & 0xf)
#define XE2_MIRROR_L3BANK_ENABLE 0x9130 /* Xe3: the banks of a node, a bit a bank */
/* XEHP_FUSE4: the compute engines present, and whether the compute front
 * end can preempt mid-thread */
#define XE2_CCS_EN(v) (((v) >> 16) & 0xf)
#define XE2_CFEG_WMTP_DISABLE (1u << 20)
/* Xe3P: the service copy engines (BCS1-8) the fuses leave */
#define XE3P_SERVICE_COPY_ENABLE 0x9170

/* ---- page attributes ------------------------------------------------------------ */

/* Beside the table's 32 entries (XEHP_PAT_INDEX): the attributes of the
 * device's address translation services, and the ones the hardware uses
 * for its own reads of page tables (PTA) and of page tables of the
 * translation table (TR_PTA, Xe3P). */
#define XE2_PAT_ATS 0x47fc
#define XE2_PAT_PTA 0x4820
#define XE3P_PAT_TR_PTA 0x48cc
#define XE2_NO_PROMOTE (1u << 10)
#define XE2_COMP_EN (1u << 9)
#define XE2_L3_CLOS(v) ((uint32_t)(v) << 6)
#define XE2_L3_POLICY(v) ((uint32_t)(v) << 4) /* 0 WB, 1 XD, 2 XA (Xe3P), 3 UC */
#define XE2_L4_POLICY(v) ((uint32_t)(v) << 2) /* 0 WB, 1 WT, 3 UC */
#define XE2_COH_MODE(v) ((uint32_t)(v) << 0) /* 0 none, 2 one-way, 3 two-way */
#define XE2_PAT_ENTRIES 32

/* page table entries: the fifth index bit, and the third one of a
 * directory entry that maps a large page */
#define XE2_PPGTT_PTE_PAT4 (1ULL << 61)
#define XE2_PPGTT_PDE_PDPE_PAT2 (1ULL << 12)

/* ---- the GuC ----------------------------------------------------------------------- */

/* the shim's cache control entry for the GuC's own accesses */
#define GUC_MOCS_INDEX(v) (((uint32_t)(v) & 0xf) << 24)
/* the GuC's translation invalidation, Xe2 on, when it is not asked of the
 * GuC itself */
#define XE2_GUC_TLB_INV_DESC0 0xcf7c
#define XE2_GUC_TLB_INV_DESC0_VALID (1u << 0)
#define XE2_GUC_TLB_INV_DESC1 0xcf80
#define XE2_GUC_TLB_INV_DESC1_INVALIDATE (1u << 6)
/* read before a translation invalidation, so the GuC sees the entries
 * the processor wrote last */
#define XE2_VF_CAP_REG 0x1901f8
/* Xe3P: the fault queues of the main GAM controller instead of the legacy
 * ones, chosen before the GuC is loaded */
#define XE3P_MAIN_GAMCTRL_MODE 0xef00
#define XE3P_MAIN_GAMCTRL_QUEUE_SELECT (1u << 0)
#define XE3_GUC_INTR_CHICKEN 0xc50c
#define XE3_DISABLE_SIGNALING_ENGINES (1u << 1)

/* ---- the engines ------------------------------------------------------------------ */

#define XE2_CSFE_CHICKEN1(base) ((base) + 0xd4) /* masked */
#define XE2_GHWSP_CSB_REPORT_DIS (1u << 15)
#define XE2_PPHWSP_CSB_AND_TIMESTAMP_REPORT_DIS (1u << 14)
#define XE2_CS_PRIORITY_MEM_READ (1u << 7)
#define XE2_RING_PWRCTX_MAXCNT(base) ((base) + 0x54)
#define XE2_IDLE_WAIT_TIME 0x000fffffu /* bits 19:0, units of 640 ns */
#define XE2_RING_IDLEDLY(base) ((base) + 0x23c)
#define XE2_INHIBIT_SWITCH_UNTIL_PREEMPTED (1u << 31)
#define XE2_IDLE_DELAY 0x001fffffu /* bits 20:0 */
#define XE2_CSBE_DEBUG_STATUS(base) ((base) + 0x3fc)
#define XE2_RC_SEMA_IDLE_MSG_DISABLE (1u << 12) /* RING_PSMI_CTL, masked */

/* the base of BCS8, the service copy engine Xe2 adds (not driven) */
#define XE2_BCS8_RING_BASE 0x3ee000

/* ---- the GT-wide workaround and tuning registers -------------------------------- */

#define XE2_STATELESS_COMPRESSION_CTRL 0x4148 /* MCR */
#define XE2_UNIFIED_COMPRESSION_FORMAT 0xfu /* bits 3:0 */
#define XE2_GAMREQSTRM_CTRL 0x4194 /* MCR */
#define XE2_CG_DIS_CNTLBUS (1u << 6)
#define XE3P_GAMSTLB_CTRL 0x477c /* MCR */
#define XE3P_DIS_PEND_GPA_LINK (1u << 13)
#define XE3P_BANK_HASH_MODE (3u << 26)
#define XE3P_BANK_HASH_4KB_MODE (3u << 26)
#define XE3P_GAMSTLB_CTRL2 0x4788 /* MCR */
#define XE3P_STLB_SINGLE_BANK_MODE (1u << 11)
#define XE3P_MMIOATSREQLIMIT_GAM_WALK_3D 0x47f8 /* MCR */
#define XE3P_DIS_ATS_WRONLY_PG (1u << 18)
#define XE2_CCCHKNREG1 0x8828 /* MCR */
#define XE2_ENCOMPPERFFIX (1u << 18)
#define XE2_L3CMPCTRL (1u << 23)
#define XE2LPM_CCCHKNREG1 0x82a8 /* the media GT's: a single register */
#define XE2_FLAT_CCS_BASE_RANGE_LOWER 0x8800 /* MCR */
#define XE2_FLAT_CCS_ENABLE (1u << 0)
#define XE3_UNSLCGCTL9454 0x9454
#define XE3_LSCFE_CLKGATE_DIS (1u << 4)
#define XE2_LSN_VC_REG2 0xb0c8 /* MCR */
#define XE2_LSN_LNI_WGT(v) ((uint32_t)(v) << 28)
#define XE2_LSN_LNE_WGT(v) ((uint32_t)(v) << 24)
#define XE2_LSN_DIM_X_WGT(v) ((uint32_t)(v) << 20)
#define XE2_LSN_DIM_Y_WGT(v) ((uint32_t)(v) << 16)
#define XE2_LSN_DIM_Z_WGT(v) ((uint32_t)(v) << 12)
#define XE2_LSN_WGT_MASK 0xfffff000u /* bits 31:12, the five fields */
#define XE2_L3SQCREG2 0xb104 /* MCR */
#define XE3P_L3_SQ_DISABLE_COAMA_2WAY_COH (1u << 30)
#define XE3P_L3_SQ_DISABLE_COAMA (1u << 22)
#define XE2_COMPMEMRD256BOVRFETCHEN (1u << 20)
#define XE2_L3SQCREG3 0xb108 /* MCR */
#define XE2_COMPPWOVERFETCHEN (1u << 28)
#define XE2_SCRATCH3_LBCF 0xb154 /* MCR */
#define XE2_RWFLUSHALLEN (1u << 17)
#define XE2_L3CLOS_MASK(i) (0xb194 + (i) * 8) /* MCR */
#define XE3P_L2COMPUTESIDECTRL 0xb1c0 /* MCR */
#define XE3P_CECTRL (3u << 1)
#define XE3P_CECTRL_CENODATA_ALWAYS (0u << 1)
#define XE2_GLOBAL_INVAL 0xb404
#define XE2_TDF_CTRL 0xb418
#define XE2_TRANSIENT_FLUSH_REQUEST (1u << 0)
/* the media GT's copies of the L3 registers (MCR on the media GT) */
#define XE2LPM_L3SQCREG2 0xb604
#define XE2LPM_L3SQCREG3 0xb608
#define XE2LPM_SCRATCH3_LBCF 0xb654
#define XE2LPM_L3SQCREG5 0xb658

/* the video engines' clock gating */
#define XE2_VDBOX_CGCTL3F08(base) ((base) + 0x3f08)
#define XE2_CG3DDISHRS_CLKGATE_DIS (1u << 5)
#define XE2_RAMDFTUNIT_CLKGATE_DIS (1u << 9) /* VDBOX_CGCTL3F10 */

/* ---- the render and compute engines' registers ------------------------------------- */

#define XE2_WM_CHICKEN3 0x5588 /* MCR, masked */
#define XE2_HIZ_PLANE_COMPRESSION_DIS (1u << 10)
#define XE2_CHICKEN_RASTER_1 0x6204 /* MCR, masked */
#define XE2_DIS_CLIP_NEGATIVE_BOUNDING_BOX (1u << 6)
#define XE2_FF_MODE 0x6210 /* MCR */
#define XE2_VS_HIT_MAX_VALUE_MASK (0x3fu << 20)
#define XE2_DIS_MESH_PARTIAL_AUTOSTRIP (1u << 16)
#define XE2_DIS_MESH_AUTOSTRIP (1u << 15)
#define XE3_DIS_TE_PATCH_CTRL (1u << 4)
#define XE2_DIS_PARTIAL_AUTOSTRIP (1u << 9) /* VFLSKPD, masked */
#define XE2_DIS_AUTOSTRIP (1u << 6)
#define XE2_DISABLE_BOTTOM_CLIP_RECTANGLE_TEST (1u << 14) /* COMMON_SLICE_CHICKEN1, MCR on Xe2 */
#define XE2_FLSH_IGNORES_PSD (1u << 10) /* XEHP_PSS_CHICKEN */
#define XE2_SBE_PUSH_CONSTANT_BEHIND_FIX_ENABLE (1u << 12) /* COMMON_SLICE_CHICKEN4, MCR */
#define XE3_HW_FILTERING (1u << 5)
#define XE3_FAST_CLEAR_VALIGN_FIX (1u << 13) /* SLICE_COMMON_ECO_CHICKEN1 */
#define XE2_VF_SCRATCHPAD 0x83a8 /* masked */
#define XE2_VFG_TED_CREDIT_INTERFACE_DISABLE (1u << 13)
#define XE2_HALF_SLICE_CHICKEN7 0xe194 /* MCR, masked */
#define XE2_CLEAR_OPTIMIZATION_DISABLE (1u << 6)
#define XE3_SMP_WAIT_FETCH_MERGING_COUNTER (3u << 10) /* SAMPLER_MODE */
#define XE3_SMP_FORCE_128B_OVERFETCH (1u << 10)
#define XE2_DISABLE_TDL_PUSH (1u << 9) /* ROW_CHICKEN4 */
#define XE2_EUPEND_CHK_FLUSH_DIS (1u << 14) /* ROW_CHICKEN3 */
#define XE3P_DIS_EU_GRF_POISON_TO_LSC (1u << 13)
#define XE2_TDL_TSL_CHICKEN 0xe4c4 /* MCR, masked */
#define XE2_STK_ID_RESTRICT (1u << 12)
#define XE2_SLM_WMTP_RESTORE (1u << 11)
#define XE2_RES_CHK_SPR_DIS (1u << 6)
#define XE3P_TDL_TSL_CHICKEN2 0xe4cc /* MCR, masked */
#define XE3P_TILEY_LOCALID (1u << 2)
#define XE2_ROW_CHICKEN 0xe4f0 /* MCR, masked */
#define XE2_EARLY_EOT_DIS (1u << 1)
#define XE2_DIS_NULL_QUERY (1u << 10) /* RT_CTRL */
#define XE2_TDL_CHICKEN 0xe5f4 /* MCR, masked */
#define XE3P_BIT_APQ_OPT_DIS (1u << 14)
#define XE3_QID_WAIT_FOR_THREAD_NOT_RUN_DISABLE (1u << 12)
#define XE2_EUSTALL_PERF_SAMPLING_DISABLE (1u << 5)
/* LSC_CHICKEN_BIT_0 and its upper half */
#define XE2_WR_REQ_CHAINING_DIS (1u << 26)
#define XE2_SEQUENTIAL_ACCESS_UPGRADE_DISABLE (1u << 13)
#define XE2_L3_128B_256B_WRT_DIS (1u << (40 - 32))
#define XE2_ALLOC_DPA_STARVE_FIX_DIS (1u << (47 - 32))
#define XE3P_SAMPLER_LD_LSC_DISABLE (1u << (45 - 32))
#define XE2_ENABLE_SMP_LD_RENDER_SURFACE_CONTROL (1u << (44 - 32))
#define XE3P_LSCFE_SAME_ADDRESS_ATOMICS_COALESCING_DISABLE (1u << (35 - 32))
#define XE3P_ROW_CHICKEN5 0xe7f0 /* MCR, masked */
#define XE3P_CPSS_AWARE_DIS (1u << 3)

/* ---- the GT -------------------------------------------------------------------------- */

/* Wa_15015404425: four writes here ahead of every register read */
#define XE2_MMIO_FLUSH_DUMMY_REG 0x130030

/* the tile's interrupt master on the discrete parts */
#define DG1_MSTR_TILE_INTR 0x190008
#define DG1_MSTR_IRQ (1u << 31)
#define DG1_MSTR_TILE(t) (1u << (t))

/* The copy engine's compression-state copy: a page's state is 8 bytes
 * (a byte per 512); "indirect" reaches the state of the memory an
 * address names, "direct" the bytes at the address themselves. */
#define XY_CTRL_SURF_COPY_BLT ((2u << 29) | (0x48u << 22) | 3)
#define XY_CTRL_SURF_SRC_DIRECT (1u << 21)
#define XY_CTRL_SURF_DST_DIRECT (1u << 20)
#define XE2_XY_CTRL_SURF_PAGES(n) ((((uint32_t)(n) - 1) & 0x3ff) << 9) /* 1..1024 */
#define XE2_XY_CTRL_SURF_MOCS(i) ((uint32_t)(i) << 28)
#define XE2_CCS_BYTES_PER_PAGE 8
#define XE2_PAT_UC_COMP 12 /* uncached, compressed: in every Xe2/Xe3/Xe3P table */

#endif
