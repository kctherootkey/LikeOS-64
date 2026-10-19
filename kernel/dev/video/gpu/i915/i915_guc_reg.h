// LikeOS -- registers of the GuC and of what it reads or is told about.
//
// The GuC and its DMA engine are programmed through registers below
// 0x40000, which a standalone media GT (Meteor Lake) has a copy of at
// I915_MEDIA_GT_BASE above the primary GT's: the code adds the GuC's GT
// offset to those.  The registers at 0x40000 and above (the host
// interrupt and scratch registers of the GuC message interface, the
// shared interrupt enables, the frequency capabilities) exist once.
//
// Every definition is guarded: i915_reg.h may name the same register.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2014-2024 Intel Corporation

#ifndef KERNEL_DEV_GPU_I915_GUC_REG_H
#define KERNEL_DEV_GPU_I915_GUC_REG_H

/* ---- the microcontroller's status and boot ROM --------------------------------- */

#ifndef GUC_STATUS
#define GUC_STATUS 0xc000
#endif
#ifndef GS_MIA_IN_RESET
#define GS_MIA_IN_RESET (1u << 0)
#endif
#ifndef GS_BOOTROM_SHIFT
#define GS_BOOTROM_SHIFT 1
#endif
#ifndef GS_BOOTROM_MASK
#define GS_BOOTROM_MASK (0x7fu << 1)
#endif
#ifndef GS_UKERNEL_SHIFT
#define GS_UKERNEL_SHIFT 8
#endif
#ifndef GS_UKERNEL_MASK
#define GS_UKERNEL_MASK (0xffu << 8)
#endif
#ifndef GS_MIA_SHIFT
#define GS_MIA_SHIFT 16
#endif
#ifndef GS_MIA_MASK
#define GS_MIA_MASK (7u << 16)
#endif
#ifndef GS_AUTH_STATUS_SHIFT
#define GS_AUTH_STATUS_SHIFT 30
#endif
#ifndef GS_AUTH_STATUS_MASK
#define GS_AUTH_STATUS_MASK (3u << 30)
#endif
#ifndef GUC_HEADER_INFO
#define GUC_HEADER_INFO 0xc014
#endif

/* The load parameters (SOFT_SCRATCH 1..14) and, before Gen11, the
 * message registers; from Gen11 the messages have registers of their
 * own above 0x40000, one set per GuC. */
#ifndef SOFT_SCRATCH
#define SOFT_SCRATCH(n) (0xc180 + (n) * 4)
#endif
#ifndef SOFT_SCRATCH_COUNT
#define SOFT_SCRATCH_COUNT 16
#endif
#ifndef GEN11_SOFT_SCRATCH
#define GEN11_SOFT_SCRATCH(n) (0x190240 + (n) * 4)
#endif
#ifndef MEDIA_SOFT_SCRATCH
#define MEDIA_SOFT_SCRATCH(n) (0x190310 + (n) * 4)
#endif
#ifndef GEN11_SOFT_SCRATCH_COUNT
#define GEN11_SOFT_SCRATCH_COUNT 4
#endif

#ifndef UOS_RSA_SCRATCH
#define UOS_RSA_SCRATCH(i) (0xc200 + (i) * 4)
#endif
#ifndef UOS_RSA_SCRATCH_COUNT
#define UOS_RSA_SCRATCH_COUNT 64
#endif

/* ---- the DMA engine that copies an image into the WOPCM ------------------------- */

#ifndef DMA_ADDR_0_LOW
#define DMA_ADDR_0_LOW 0xc300
#endif
#ifndef DMA_ADDR_0_HIGH
#define DMA_ADDR_0_HIGH 0xc304
#endif
#ifndef DMA_ADDR_1_LOW
#define DMA_ADDR_1_LOW 0xc308
#endif
#ifndef DMA_ADDR_1_HIGH
#define DMA_ADDR_1_HIGH 0xc30c
#endif
#ifndef DMA_ADDRESS_SPACE_WOPCM
#define DMA_ADDRESS_SPACE_WOPCM (7u << 16)
#endif
#ifndef DMA_COPY_SIZE
#define DMA_COPY_SIZE 0xc310
#endif
#ifndef DMA_CTRL
#define DMA_CTRL 0xc314
#endif
#ifndef HUC_UKERNEL
#define HUC_UKERNEL (1u << 9)
#endif
#ifndef UOS_MOVE
#define UOS_MOVE (1u << 4)
#endif
#ifndef START_DMA
#define START_DMA (1u << 0)
#endif

/* ---- the WOPCM partition -------------------------------------------------------- */

#ifndef DMA_GUC_WOPCM_OFFSET
#define DMA_GUC_WOPCM_OFFSET 0xc340
#endif
#ifndef GUC_WOPCM_OFFSET_VALID
#define GUC_WOPCM_OFFSET_VALID (1u << 0)
#endif
#ifndef HUC_LOADING_AGENT_GUC
#define HUC_LOADING_AGENT_GUC (1u << 1)
#endif
#ifndef GUC_WOPCM_OFFSET_SHIFT
#define GUC_WOPCM_OFFSET_SHIFT 14
#endif
#ifndef GUC_WOPCM_OFFSET_MASK
#define GUC_WOPCM_OFFSET_MASK (0x3ffffu << 14)
#endif
#ifndef GUC_WOPCM_SIZE
#define GUC_WOPCM_SIZE 0xc050
#endif
#ifndef GUC_WOPCM_SIZE_LOCKED
#define GUC_WOPCM_SIZE_LOCKED (1u << 0)
#endif
#ifndef GUC_WOPCM_SIZE_MASK
#define GUC_WOPCM_SIZE_MASK (0xfffffu << 12)
#endif

/* ---- the shim between the microcontroller and memory ---------------------------- */

#ifndef GUC_SHIM_CONTROL
#define GUC_SHIM_CONTROL 0xc064
#endif
#ifndef GUC_DISABLE_SRAM_INIT_TO_ZEROES
#define GUC_DISABLE_SRAM_INIT_TO_ZEROES (1u << 0)
#endif
#ifndef GUC_ENABLE_READ_CACHE_LOGIC
#define GUC_ENABLE_READ_CACHE_LOGIC (1u << 1)
#endif
#ifndef GUC_ENABLE_MIA_CACHING
#define GUC_ENABLE_MIA_CACHING (1u << 2)
#endif
#ifndef GUC_GEN10_MSGCH_ENABLE
#define GUC_GEN10_MSGCH_ENABLE (1u << 4)
#endif
#ifndef GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA
#define GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA (1u << 9)
#endif
#ifndef GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA
#define GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA (1u << 10)
#endif
#ifndef GUC_ENABLE_MIA_CLOCK_GATING
#define GUC_ENABLE_MIA_CLOCK_GATING (1u << 15)
#endif
#ifndef GUC_SHIM_CONTROL2
#define GUC_SHIM_CONTROL2 0xc068
#endif
#ifndef GUC_ENABLE_DEBUG_REG
#define GUC_ENABLE_DEBUG_REG (1u << 11)
#endif
#ifndef GUC_IS_PRIVILEGED
#define GUC_IS_PRIVILEGED (1u << 29)
#endif
#ifndef GSC_LOADS_HUC
#define GSC_LOADS_HUC (1u << 30)
#endif

#ifndef GEN9_GT_PM_CONFIG
#define GEN9_GT_PM_CONFIG 0x13816c
#endif
#ifndef GEN9LP_GT_PM_CONFIG
#define GEN9LP_GT_PM_CONFIG 0x138140
#endif
#ifndef GT_DOORBELL_ENABLE
#define GT_DOORBELL_ENABLE (1u << 0)
#endif
#ifndef GEN7_MISCCPCTL
#define GEN7_MISCCPCTL 0x9424
#endif
#ifndef GEN8_DOP_CLOCK_GATE_GUC_ENABLE
#define GEN8_DOP_CLOCK_GATE_GUC_ENABLE (1u << 4)
#endif
#ifndef GUC_ARAT_C6DIS
#define GUC_ARAT_C6DIS 0xa178
#endif

/* ---- doorbells to the GuC and from it ------------------------------------------- */

#ifndef GUC_SEND_INTERRUPT
#define GUC_SEND_INTERRUPT 0xc4c8
#endif
#ifndef GUC_SEND_TRIGGER
#define GUC_SEND_TRIGGER (1u << 0)
#endif
#ifndef GEN11_GUC_HOST_INTERRUPT
#define GEN11_GUC_HOST_INTERRUPT 0x1901f0
#endif
#ifndef MEDIA_GUC_HOST_INTERRUPT
#define MEDIA_GUC_HOST_INTERRUPT 0x190304
#endif
#ifndef GUC_INTR_GUC2HOST
#define GUC_INTR_GUC2HOST (1u << 15)
#endif

/* The GuC-to-host interrupt: one enable for both GuCs (the upper half),
 * one mask register whose upper half is the primary GuC's and whose
 * lower half is the media GuC's. */
#ifndef GEN11_GUC_SG_INTR_ENABLE
#define GEN11_GUC_SG_INTR_ENABLE 0x190038
#endif
#ifndef GEN11_GUC_SG_INTR_MASK
#define GEN11_GUC_SG_INTR_MASK 0x1900e8
#endif
#ifndef MTL_GUC_MGUC_INTR_MASK
#define MTL_GUC_MGUC_INTR_MASK 0x1900e8
#endif
#ifndef GEN8_GT_IIR
#define GEN8_GT_IIR(n) (0x44308 + (n) * 0x10)
#endif
#ifndef GEN8_GT_IMR
#define GEN8_GT_IMR(n) (0x44304 + (n) * 0x10)
#endif
#ifndef GEN8_GT_IER
#define GEN8_GT_IER(n) (0x4430c + (n) * 0x10)
#endif

/* MI_SEMAPHORE_WAIT signals reach the GuC (which owns the scheduling)
 * rather than the host. */
#ifndef GEN12_GUC_SEM_INTR_ENABLES
#define GEN12_GUC_SEM_INTR_ENABLES 0xc71c
#endif
#ifndef GUC_SEM_INTR_ROUTE_TO_GUC
#define GUC_SEM_INTR_ROUTE_TO_GUC (1u << 31)
#endif
#ifndef GUC_SEM_INTR_ENABLE_ALL
#define GUC_SEM_INTR_ENABLE_ALL 0xffu
#endif

/* ---- translation, reset ----------------------------------------------------------- */

#ifndef GEN8_GTCR
#define GEN8_GTCR 0x4274
#endif
#ifndef GEN8_GTCR_INVALIDATE
#define GEN8_GTCR_INVALIDATE (1u << 0)
#endif
#ifndef GEN12_GUC_TLB_INV_CR
#define GEN12_GUC_TLB_INV_CR 0xcee8
#endif
#ifndef GEN12_GUC_TLB_INV_CR_INVALIDATE
#define GEN12_GUC_TLB_INV_CR_INVALIDATE (1u << 0)
#endif
#ifndef GEN6_GDRST
#define GEN6_GDRST 0x941c
#endif
#ifndef GEN9_GRDOM_GUC
#define GEN9_GRDOM_GUC (1u << 5)
#endif
#ifndef GEN11_GRDOM_GUC
#define GEN11_GRDOM_GUC (1u << 3)
#endif

/* The doorbell count per SQIDI, for the GT system information. */
#ifndef GEN12_DIST_DBS_POPULATED
#define GEN12_DIST_DBS_POPULATED 0xd08
#endif
#ifndef GEN12_DOORBELLS_PER_SQIDI_SHIFT
#define GEN12_DOORBELLS_PER_SQIDI_SHIFT 16
#endif
#ifndef GEN12_DOORBELLS_PER_SQIDI
#define GEN12_DOORBELLS_PER_SQIDI 0xffu
#endif

/* ---- the HuC ------------------------------------------------------------------------ */

#ifndef HUC_STATUS2
#define HUC_STATUS2 0xd3b0
#endif
#ifndef HUC_FW_VERIFIED
#define HUC_FW_VERIFIED (1u << 7)
#endif
#ifndef GEN11_HUC_KERNEL_LOAD_INFO
#define GEN11_HUC_KERNEL_LOAD_INFO 0xc1dc
#endif
#ifndef HUC_LOAD_SUCCESSFUL
#define HUC_LOAD_SUCCESSFUL (1u << 0)
#endif

/* ---- frequencies (SLPC) ------------------------------------------------------------- */

#ifndef GEN6_PMINTRMSK
#define GEN6_PMINTRMSK 0xa168
#endif
#ifndef ARAT_EXPIRED_INTRMSK
#define ARAT_EXPIRED_INTRMSK (1u << 9)
#endif
#ifndef GEN6_RP_STATE_CAP
#define GEN6_RP_STATE_CAP 0x145998
#endif
#ifndef GEN10_FREQ_INFO_REC
#define GEN10_FREQ_INFO_REC 0x145ef0
#endif
#ifndef RPE_MASK
#define RPE_MASK (0xffu << 8)
#endif
/* Meteor Lake: the capabilities of each GT, in units of 50/3 MHz */
#ifndef MTL_RP_STATE_CAP
#define MTL_RP_STATE_CAP 0x138000
#endif
#ifndef MTL_MEDIAP_STATE_CAP
#define MTL_MEDIAP_STATE_CAP 0x138020
#endif
#ifndef MTL_RP0_CAP_MASK
#define MTL_RP0_CAP_MASK 0x1ffu
#endif
#ifndef MTL_RPN_CAP_SHIFT
#define MTL_RPN_CAP_SHIFT 16
#endif
#ifndef MTL_RPN_CAP_MASK
#define MTL_RPN_CAP_MASK (0x1ffu << 16)
#endif
#ifndef MTL_GT_RPE_FREQUENCY
#define MTL_GT_RPE_FREQUENCY 0x13800c
#endif
#ifndef MTL_MPE_FREQUENCY
#define MTL_MPE_FREQUENCY 0x13802c
#endif
#ifndef MTL_RPE_MASK
#define MTL_RPE_MASK 0x1ffu
#endif

/* ---- registers the GuC saves and restores around an engine reset ------------------ */

#ifndef RING_MODE_GEN7
#define RING_MODE_GEN7(base) ((base) + 0x29c)
#endif
#ifndef RING_HWS_PGA
#define RING_HWS_PGA(base) ((base) + 0x80)
#endif
#ifndef RING_IMR
#define RING_IMR(base) ((base) + 0xa8)
#endif
#ifndef RING_FORCE_TO_NONPRIV
#define RING_FORCE_TO_NONPRIV(base, i) ((base) + 0x4d0 + (i) * 4)
#endif
#ifndef RING_MAX_NONPRIV_SLOTS
#define RING_MAX_NONPRIV_SLOTS 12
#endif
#ifndef GEN12_RCU_MODE
#define GEN12_RCU_MODE 0x14800
#endif
#ifndef GEN9_LNCFCMOCS
#define GEN9_LNCFCMOCS(i) (0xb020 + (i) * 4)
#endif
#ifndef XEHP_LNCFCMOCS
#define XEHP_LNCFCMOCS(i) (0xb020 + (i) * 4)
#endif
#ifndef LNCFCMOCS_REG_COUNT
#define LNCFCMOCS_REG_COUNT 32
#endif
#ifndef EU_PERF_CNTL0
#define EU_PERF_CNTL0 0xe458
#endif
#ifndef EU_PERF_CNTL1
#define EU_PERF_CNTL1 0xe558
#endif
#ifndef EU_PERF_CNTL2
#define EU_PERF_CNTL2 0xe658
#endif
#ifndef EU_PERF_CNTL3
#define EU_PERF_CNTL3 0xe758
#endif
#ifndef EU_PERF_CNTL4
#define EU_PERF_CNTL4 0xe45c
#endif
#ifndef EU_PERF_CNTL5
#define EU_PERF_CNTL5 0xe55c
#endif
#ifndef EU_PERF_CNTL6
#define EU_PERF_CNTL6 0xe65c
#endif

/* ---- the media engines' fuses (the GT system information) -------------------------- */

#ifndef GEN11_GT_VEBOX_VDBOX_DISABLE
#define GEN11_GT_VEBOX_VDBOX_DISABLE 0x9140
#endif
#ifndef GEN11_GT_VDBOX_DISABLE_MASK
#define GEN11_GT_VDBOX_DISABLE_MASK 0xffu
#endif
#ifndef GEN11_GT_VEBOX_DISABLE_SHIFT
#define GEN11_GT_VEBOX_DISABLE_SHIFT 16
#endif
#ifndef GEN11_GT_VEBOX_DISABLE_MASK
#define GEN11_GT_VEBOX_DISABLE_MASK (0x0fu << 16)
#endif
#ifndef HSW_PAVP_FUSE1
#define HSW_PAVP_FUSE1 0x911c
#endif
#ifndef XEHP_SFC_ENABLE
#define XEHP_SFC_ENABLE(v) (((v) >> 24) & 0xf)
#endif

/* ---- where the GuC's view of the global space ends ---------------------------------- */

#ifndef GUC_GGTT_TOP
#define GUC_GGTT_TOP 0xfee00000u
#endif

#endif
