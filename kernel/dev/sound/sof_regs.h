// LikeOS Intel audio DSP host driver - private definitions shared by
// sof_pci.c, sof_dsp.c, sof_loader.c, sof_ipc4.c and sof_pcm.c.
//
// The audio DSP of Intel PCHs/SoCs sits behind the same PCI function as the
// High Definition Audio controller.  BAR0 is the HDA register file (global
// registers, stream descriptors, the processing-pipe, SPIB, DRSM and
// multi-link capabilities, Intel vendor registers); BAR4 is the DSP's own
// register space (core power/reset, the IPC doorbells, the shared SRAM
// windows that carry IPC payloads and firmware status).  Two generations are
// handled here:
//
//   cAVS 2.5 (Tiger/Alder/Raptor Lake, Elkhart Lake): ADSPCS core control,
//            IPC doorbells at 0xC0, SRAM windows of 128 KiB at 0x80000.
//   ACE 1.x/2.x/3.x (Meteor/Arrow/Lunar/Panther Lake): DSP subsystem power
//            through HfDSSCS and HfPWRCTL, core 0 through DSP2CxCTL, IPC
//            doorbells at 0x73200, SRAM windows of 32 KiB at 0x180000.
//
// Older platforms (Skylake .. Ice Lake) are recognised and declined.
//
// struct sof_host embeds the contract's struct sof_dev as its FIRST member,
// so the struct sof_dev pointer handed to the topology and SoundWire sides is
// also a pointer to the whole host state (sof_host_of()).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_SOF_REGS_H
#define KERNEL_DEV_SOUND_SOF_REGS_H

#include "sof.h"
#include "sof_tplg.h"

#include <kernel/ke/syscall.h> /* errno values */

/* ===================================================================== */
/* HDA register file (BAR0)                                               */
/* ===================================================================== */

#define SOF_HDA_GCAP 0x00
#define SOF_HDA_GCAP_64OK (1u << 0)
#define SOF_HDA_VMIN 0x02
#define SOF_HDA_VMAJ 0x03
#define SOF_HDA_GCTL 0x08
#define SOF_HDA_GCTL_CRST (1u << 0)
#define SOF_HDA_GCTL_UNSOL (1u << 8)
#define SOF_HDA_WAKEEN 0x0c
#define SOF_HDA_STATESTS 0x0e
#define SOF_HDA_STATESTS_MASK 0x7fff
#define SOF_HDA_LLCH 0x14 /* first capability pointer */
#define SOF_HDA_INTCTL 0x20
#define SOF_HDA_INTCTL_GIE (1u << 31)
#define SOF_HDA_INTCTL_CIE (1u << 30)
#define SOF_HDA_INTCTL_SIE_MASK 0x3fffffffu
#define SOF_HDA_INTSTS 0x24
#define SOF_HDA_INTSTS_GIS (1u << 31)
#define SOF_HDA_INTSTS_CIS (1u << 30)
#define SOF_HDA_WALCLK 0x30
#define SOF_HDA_SSYNC 0x38
#define SOF_HDA_CORBCTL 0x4c
#define SOF_HDA_RIRBCTL 0x5c
#define SOF_HDA_RIRBSTS 0x5d
#define SOF_HDA_RIRBSTS_MASK 0x05
#define SOF_HDA_DPLBASE 0x70
#define SOF_HDA_DPLBASE_ENABLE (1u << 0)
#define SOF_HDA_DPUBASE 0x74

/* stream descriptors */
#define SOF_HDA_SD_BASE 0x80
#define SOF_HDA_SD_STRIDE 0x20
#define SOF_SD_CTL 0x00 /* 24 bits: CTL0..2 */
#define SOF_SD_STS 0x03
#define SOF_SD_LPIB 0x04
#define SOF_SD_CBL 0x08
#define SOF_SD_LVI 0x0c
#define SOF_SD_FIFOW 0x0e
#define SOF_SD_FIFOS 0x10
#define SOF_SD_FMT 0x12
#define SOF_SD_FIFOL 0x14
#define SOF_SD_BDLPL 0x18
#define SOF_SD_BDLPU 0x1c

#define SOF_SD_CTL_SRST (1u << 0)
#define SOF_SD_CTL_RUN (1u << 1)
#define SOF_SD_CTL_IOCE (1u << 2)
#define SOF_SD_CTL_FEIE (1u << 3)
#define SOF_SD_CTL_DEIE (1u << 4)
#define SOF_SD_CTL_INT_MASK (SOF_SD_CTL_IOCE | SOF_SD_CTL_FEIE | SOF_SD_CTL_DEIE)
#define SOF_SD_CTL_TAG_SHIFT 20
#define SOF_SD_CTL_TAG_MASK (0xfu << SOF_SD_CTL_TAG_SHIFT)

#define SOF_SD_STS_BCIS (1u << 2)
#define SOF_SD_STS_FIFOE (1u << 3)
#define SOF_SD_STS_DESE (1u << 4)
#define SOF_SD_STS_FIFORDY (1u << 5)
#define SOF_SD_STS_MASK (SOF_SD_STS_BCIS | SOF_SD_STS_FIFOE | SOF_SD_STS_DESE)

#define SOF_SD_FIFOS_MASK 0xffffu

/* Intel vendor-specific registers in BAR0 */
#define SOF_HDA_VS_EM2 0x1030
#define SOF_HDA_VS_EM2_L1SEN (1u << 13)
#define SOF_HDA_VS_LTRP 0x1048
#define SOF_HDA_VS_LTRP_GB_MASK 0x3f
#define SOF_HDA_LTRP_GB_BOOT_US 95 /* guard band while the image streams */
#define SOF_HDA_VS_D0I3C 0x104a /* cAVS */
#define SOF_ACE_VS_D0I3C 0x1d4a /* ACE */
#define SOF_VS_D0I3C_CIP (1u << 0)
#define SOF_VS_D0I3C_I3 (1u << 2)
#define SOF_HDA_VS_DPIB_BASE 0x1084
#define SOF_HDA_VS_DPIB_STRIDE 0x20
#define SOF_HDA_VS_EFIFOS_BASE 0x1094

/* PCI configuration space (Intel audio functions) */
#define SOF_PCI_TCSEL 0x44
#define SOF_PCI_PGCTL 0x44
#define SOF_PCI_PGCTL_ADSPPGD (1u << 2)
#define SOF_PCI_PGCTL_LSRMD (1u << 4)
#define SOF_PCI_CGCTL 0x48
#define SOF_PCI_CGCTL_ADSPDCGE (1u << 1)
#define SOF_PCI_CGCTL_MISCBDCGE (1u << 6)

/* extended capability list in BAR0 */
#define SOF_CAP_ID_SHIFT 16
#define SOF_CAP_ID_MASK (0xfffu << SOF_CAP_ID_SHIFT)
#define SOF_CAP_NEXT_MASK 0xffffu
#define SOF_CAP_ID_GTS 0x1
#define SOF_CAP_ID_ML 0x2
#define SOF_CAP_ID_PP 0x3
#define SOF_CAP_ID_SPB 0x4
#define SOF_CAP_ID_DRSM 0x5
#define SOF_CAP_MAX_WALK 16

/* processing pipe capability (offsets from the PP capability) */
#define SOF_PP_PPCH 0x00
#define SOF_PP_PPCTL 0x04
#define SOF_PP_PPCTL_PIE (1u << 31) /* DSP interrupts reach INTSTS */
#define SOF_PP_PPCTL_GPROCEN (1u << 30) /* DSP processing enable */
#define SOF_PP_PPCTL_PROCEN(i) (1u << (i)) /* stream i decoupled */
#define SOF_PP_PPSTS 0x08
#define SOF_PP_PPHC_BASE 0x10 /* host-side position: per stream 0x10 */
#define SOF_PP_PPHC_STRIDE 0x10
#define SOF_PP_PPLC_BASE 0x10 /* link side: after nstreams * 0x10 */
#define SOF_PP_PPLC_STRIDE 0x10
#define SOF_PPLC_CTL 0x00
#define SOF_PPLC_CTL_STRST (1u << 0)
#define SOF_PPLC_CTL_RUN (1u << 1)
#define SOF_PPLC_CTL_STRM_SHIFT 20
#define SOF_PPLC_CTL_STRM_MASK (0xfu << SOF_PPLC_CTL_STRM_SHIFT)
#define SOF_PPLC_FMT 0x04
#define SOF_PPLC_LLPL 0x08
#define SOF_PPLC_LLPU 0x0c

/* software position in buffer capability */
#define SOF_SPB_SPBFCH 0x00
#define SOF_SPB_SPBFCCTL 0x04
#define SOF_SPB_BASE 0x08
#define SOF_SPB_STRIDE 0x08
#define SOF_SPB_SPIB 0x00
#define SOF_SPB_MAXFIFO 0x04

/* multi-link capability */
#define SOF_ML_MLCH 0x00
#define SOF_ML_MLCD 0x04
#define SOF_ML_LINK_BASE 0x40
#define SOF_ML_LINK_STRIDE 0x40
#define SOF_ML_LCAP 0x00
#define SOF_ML_LCAP_ALT (1u << 28)
#define SOF_ML_LCAP_INTC (1u << 27)
#define SOF_ML_LCAP_OFLS (1u << 26)
#define SOF_ML_LCAP_LSS (1u << 23)
#define SOF_ML_LCAP_SLCOUNT_SHIFT 20
#define SOF_ML_LCAP_SLCOUNT_MASK (0x7u << SOF_ML_LCAP_SLCOUNT_SHIFT)
#define SOF_ML_LCTL 0x04
#define SOF_ML_LCTL_INTSTS (1u << 31)
#define SOF_ML_LCTL_CPA_SHIFT 23
#define SOF_ML_LCTL_SPA_SHIFT 16
#define SOF_ML_LCTL_INTEN (1u << 5)
#define SOF_ML_LCTL_OFLEN (1u << 4)
#define SOF_ML_LOSIDV 0x08
#define SOF_ML_LSDIID 0x0c
#define SOF_ML_LEPTR 0x20
#define SOF_ML_LEPTR_ID_SHIFT 24
#define SOF_ML_LEPTR_PTR_MASK 0xfffffu
#define SOF_ML_ID_SDW 0x00
#define SOF_ML_ID_SSP 0xc0
#define SOF_ML_ID_DMIC 0xc1
#define SOF_ML_ID_UAOL 0xc2
#define SOF_ML_MAX_LINKS 8
/* SoundWire alternate link: per-sublink SHIM, PCM stream channel map */
#define SOF_ML_SDW_INSTANCE_STRIDE 0x8000
#define SOF_ML_SDW_PCMSYCM(y) (0x16 + 0x4 * (y))

/* ===================================================================== */
/* DSP register file (BAR4)                                               */
/* ===================================================================== */

/* cAVS */
#define SOF_CAVS_ADSPCS 0x04
#define SOF_CAVS_ADSPCS_CRST(m) ((uint32_t)(m) << 0)
#define SOF_CAVS_ADSPCS_CSTALL(m) ((uint32_t)(m) << 8)
#define SOF_CAVS_ADSPCS_SPA(m) ((uint32_t)(m) << 16)
#define SOF_CAVS_ADSPCS_CPA(m) ((uint32_t)(m) << 24)
#define SOF_CAVS_ADSPIC 0x08
#define SOF_CAVS_ADSPIS 0x0c
#define SOF_CAVS_ADSPIC2 0x10
#define SOF_CAVS_ADSPIS2 0x14
#define SOF_CAVS_ADSPIX_IPC (1u << 0)
#define SOF_CAVS_ADSPIX_CLDMA (1u << 1)
#define SOF_CAVS_ADSPIX2_SNDW (1u << 5)
#define SOF_CAVS_IPC_TDR 0xc0
#define SOF_CAVS_IPC_TDA 0xc4
#define SOF_CAVS_IPC_TDD 0xc8
#define SOF_CAVS_IPC_IDR 0xd0
#define SOF_CAVS_IPC_IDA 0xd4
#define SOF_CAVS_IPC_IDD 0xd8
#define SOF_CAVS_IPC_CTL 0xe8
#define SOF_CAVS_SRAM_WINDOW(x) (0x80000u + (uint32_t)(x) * 0x20000u)
#define SOF_CAVS_ROM_STATUS 0x80000u
#define SOF_CAVS_SSP_BASE 0x10000u
#define SOF_CAVS_SSP_STRIDE 0x1000u
#define SOF_SSP_SSC1 0x4
#define SOF_SSP_SSC1_CBP_CFP ((1u << 25) | (1u << 24))

/* ACE */
#define SOF_ACE_HFDSSCS 0x1000
#define SOF_ACE_HFDSSCS_SPA (1u << 16)
#define SOF_ACE_HFDSSCS_CPA (1u << 24)
#define SOF_ACE_HFINTIPPTR 0x1108
#define SOF_ACE_HFINTIPPTR_MASK 0x1fffffu
#define SOF_ACE_INTEN_IPC (1u << 0) /* at *HFINTIPPTR */
#define SOF_ACE_INTEN_SDW (1u << 6)
#define SOF_ACE_IRQSTS 0x20 /* added to *HFINTIPPTR */
#define SOF_ACE_IRQSTS_IPC (1u << 0)
#define SOF_ACE_IRQSTS_SDW (1u << 6)
#define SOF_ACE_HFHIPCIE 0x1140
#define SOF_ACE_HFHIPCIE_IE (1u << 0)
#define SOF_ACE_HFSNDWIE 0x114c
#define SOF_ACE_HFSNDWIE_MASK 0xfu
#define SOF_ACE_HFPWRCTL 0x1d18
#define SOF_ACE_HFPWRSTS 0x1d1c
#define SOF_ACE3_HFPWRCTL2 0x1d20
#define SOF_ACE3_HFPWRSTS2 0x1d24
#define SOF_ACE_HFPWRCTL_WPDSPHPXPG (1u << 0)
#define SOF_ACE_HFPWRCTL_WPIOXPG(x) (1u << ((x) + 8))
#define SOF_ACE_HFPWRSTS_DSPHPXPGS (1u << 0)
#define SOF_ACE_DSP2CXCAP 0x178d00
#define SOF_ACE_DSP2CXCTL 0x178d04
#define SOF_ACE_DSP2CXCTL_SPA (1u << 0)
#define SOF_ACE_DSP2CXCTL_CPA (1u << 8)
#define SOF_ACE_DSP2CXCTL_OSEL_SHIFT 24
#define SOF_ACE_DSP2CXCTL_OSEL_MASK (3u << 24)
#define SOF_ACE_DSP2CXCTL_OSEL_HOST 2u
#define SOF_ACE_IPC_TDR 0x73200
#define SOF_ACE_IPC_TDA 0x73204
#define SOF_ACE_IPC_IDR 0x73210
#define SOF_ACE_IPC_IDA 0x73214
#define SOF_ACE_IPC_CTL 0x73228
#define SOF_ACE_IPC_TDD 0x73300
#define SOF_ACE_IPC_IDD 0x73380
#define SOF_ACE_SRAM_WINDOW(x) (0x180000u + (uint32_t)(x) * 0x8000u)
#define SOF_ACE1_ROM_STATUS 0x163200u /* HfFLGPxQWy: core 0 status */
#define SOF_ACE2_ROM_STATUS 0x160200u /* HfDSC */
#define SOF_ACE_SSP_BASE 0x10000u

/* doorbell bits common to both generations */
#define SOF_IPC_BUSY (1u << 31) /* TDR/IDR */
#define SOF_IPC_MSG_MASK 0x7fffffffu
#define SOF_IPC_DONE (1u << 31) /* IDA (both), TDA (cAVS) */
#define SOF_IPC_CTL_BUSY (1u << 0)
#define SOF_IPC_CTL_DONE (1u << 1)

/* ROM control message (before firmware) */
#define SOF_ROM_IPC_CONTROL 0x01000000u
#define SOF_ROM_IPC_PURGE_FW 0x00004000u
#define SOF_ROM_IPC_DMA_SHIFT 9

/* firmware status register (FSR) */
#define SOF_FSR_STATE_MASK 0x00ffffffu
#define SOF_FSR_WAIT_SHIFT 24
#define SOF_FSR_WAIT_MASK (0xfu << SOF_FSR_WAIT_SHIFT)
#define SOF_FSR_MODULE_SHIFT 28
#define SOF_FSR_MODULE_MASK (0x7u << SOF_FSR_MODULE_SHIFT)
#define SOF_FSR_HALTED (1u << 31)
#define SOF_FSR_STATE_INIT 0x0
#define SOF_FSR_STATE_INIT_DONE 0x1
#define SOF_FSR_STATE_FW_ENTERED 0x5

/* ===================================================================== */
/* IPC4 message encoding                                                  */
/* ===================================================================== */

#define SOF_IPC4_MAILBOX_SIZE 4096u
#define SOF_IPC4_TARGET_SHIFT 30
#define SOF_IPC4_TARGET_MODULE (1u << 30)
#define SOF_IPC4_DIR_REPLY (1u << 29)
#define SOF_IPC4_TYPE_SHIFT 24
#define SOF_IPC4_TYPE_MASK (0x1fu << SOF_IPC4_TYPE_SHIFT)
#define SOF_IPC4_TYPE(x) (((uint32_t)(x) << SOF_IPC4_TYPE_SHIFT) & SOF_IPC4_TYPE_MASK)
#define SOF_IPC4_TYPE_GET(x) (((x) & SOF_IPC4_TYPE_MASK) >> SOF_IPC4_TYPE_SHIFT)
#define SOF_IPC4_STATUS_MASK 0x00ffffffu

/* global message types */
#define SOF_IPC4_GLB_BOOT_CONFIG 0
#define SOF_IPC4_GLB_ROM_CONTROL 1
#define SOF_IPC4_GLB_IPCGATEWAY_CMD 2
#define SOF_IPC4_GLB_PERF_MEASUREMENTS_CMD 13
#define SOF_IPC4_GLB_CHAIN_DMA 14
#define SOF_IPC4_GLB_LOAD_MULTIPLE_MODULES 15
#define SOF_IPC4_GLB_UNLOAD_MULTIPLE_MODULES 16
#define SOF_IPC4_GLB_CREATE_PIPELINE 17
#define SOF_IPC4_GLB_DELETE_PIPELINE 18
#define SOF_IPC4_GLB_SET_PIPELINE_STATE 19
#define SOF_IPC4_GLB_GET_PIPELINE_STATE 20
#define SOF_IPC4_GLB_GET_PIPELINE_CONTEXT_SIZE 21
#define SOF_IPC4_GLB_SAVE_PIPELINE 22
#define SOF_IPC4_GLB_RESTORE_PIPELINE 23
#define SOF_IPC4_GLB_LOAD_LIBRARY 24
#define SOF_IPC4_GLB_LOAD_LIBRARY_PREPARE 25
#define SOF_IPC4_GLB_INTERNAL_MESSAGE 26
#define SOF_IPC4_GLB_NOTIFICATION 27

/* module message types */
#define SOF_IPC4_MOD_INIT_INSTANCE 0
#define SOF_IPC4_MOD_CONFIG_GET 1
#define SOF_IPC4_MOD_CONFIG_SET 2
#define SOF_IPC4_MOD_LARGE_CONFIG_GET 3
#define SOF_IPC4_MOD_LARGE_CONFIG_SET 4
#define SOF_IPC4_MOD_BIND 5
#define SOF_IPC4_MOD_UNBIND 6
#define SOF_IPC4_MOD_SET_DX 7
#define SOF_IPC4_MOD_SET_D0IX 8
#define SOF_IPC4_MOD_ENTER_RESTORE 9
#define SOF_IPC4_MOD_EXIT_RESTORE 10
#define SOF_IPC4_MOD_DELETE_INSTANCE 11

/* pipeline fields (primary) */
#define SOF_IPC4_PPL_INSTANCE(x) (((uint32_t)(x) & 0xffu) << 16)
#define SOF_IPC4_PPL_PRIORITY(x) (((uint32_t)(x) & 0x1fu) << 11)
#define SOF_IPC4_PPL_MEM_PAGES(x) ((uint32_t)(x) & 0x7ffu)
#define SOF_IPC4_PPL_STATE(x) ((uint32_t)(x) & 0xffffu)
/* pipeline fields (extension) */
#define SOF_IPC4_PPL_EXT_LP(x) ((uint32_t)(x) & 1u)
#define SOF_IPC4_PPL_EXT_CORE(x) (((uint32_t)(x) & 0xfu) << 20)
#define SOF_IPC4_PPL_EXT_PAYLOAD (1u << 29)
#define SOF_IPC4_PPL_STATE_EXT_MULTI (1u << 0)

#define SOF_IPC4_PIPE_RESET 2
#define SOF_IPC4_PIPE_PAUSED 3
#define SOF_IPC4_PIPE_RUNNING 4
#define SOF_IPC4_PIPE_EOS 5

/* module fields (primary) */
#define SOF_IPC4_MOD_ID(x) ((uint32_t)(x) & 0xffffu)
#define SOF_IPC4_MOD_INSTANCE(x) (((uint32_t)(x) & 0xffu) << 16)
/* module fields (extension) */
#define SOF_IPC4_MOD_EXT_PARAM_SIZE(x) ((uint32_t)(x) & 0xffffu)
#define SOF_IPC4_MOD_EXT_PPL_ID(x) (((uint32_t)(x) & 0xffu) << 16)
#define SOF_IPC4_MOD_EXT_CORE_ID(x) (((uint32_t)(x) & 0xfu) << 24)
#define SOF_IPC4_MOD_EXT_DOMAIN(x) (((uint32_t)(x) & 1u) << 28)
#define SOF_IPC4_MOD_EXT_DST_MOD_ID(x) ((uint32_t)(x) & 0xffffu)
#define SOF_IPC4_MOD_EXT_DST_INSTANCE(x) (((uint32_t)(x) & 0xffu) << 16)
#define SOF_IPC4_MOD_EXT_DST_QUEUE(x) (((uint32_t)(x) & 0x7u) << 24)
#define SOF_IPC4_MOD_EXT_SRC_QUEUE(x) (((uint32_t)(x) & 0x7u) << 27)
/* large config extension */
#define SOF_IPC4_LCFG_SIZE_MASK 0xfffffu
#define SOF_IPC4_LCFG_PARAM_SHIFT 20
#define SOF_IPC4_LCFG_PARAM_MASK (0xffu << SOF_IPC4_LCFG_PARAM_SHIFT)
#define SOF_IPC4_LCFG_PARAM(x) (((uint32_t)(x) & 0xffu) << SOF_IPC4_LCFG_PARAM_SHIFT)
#define SOF_IPC4_LCFG_LAST (1u << 28)
#define SOF_IPC4_LCFG_FIRST (1u << 29)

/* base firmware module and its parameters */
#define SOF_IPC4_BASEFW_MODULE 0
#define SOF_IPC4_BASEFW_INSTANCE 0
#define SOF_IPC4_FW_PARAM_ENABLE_LOGS 6
#define SOF_IPC4_FW_PARAM_FW_CONFIG 7
#define SOF_IPC4_FW_PARAM_HW_CONFIG 8
#define SOF_IPC4_FW_PARAM_MODULES_INFO 9
#define SOF_IPC4_FW_PARAM_LIBRARIES_INFO 16
#define SOF_IPC4_FW_PARAM_SYSTEM_TIME 20

/* FW_CONFIG tuple types */
#define SOF_IPC4_FW_CFG_FW_VERSION 0
#define SOF_IPC4_FW_CFG_MEMORY_RECLAIMED 1
#define SOF_IPC4_FW_CFG_SLOW_CLOCK_FREQ_HZ 2
#define SOF_IPC4_FW_CFG_FAST_CLOCK_FREQ_HZ 3
#define SOF_IPC4_FW_CFG_DMA_BUFFER_CONFIG 4
#define SOF_IPC4_FW_CFG_ALH_SUPPORT_LEVEL 5
#define SOF_IPC4_FW_CFG_DL_MAILBOX_BYTES 6
#define SOF_IPC4_FW_CFG_UL_MAILBOX_BYTES 7
#define SOF_IPC4_FW_CFG_TRACE_LOG_BYTES 8
#define SOF_IPC4_FW_CFG_MAX_PPL_COUNT 9
#define SOF_IPC4_FW_CFG_MAX_ASTATE_COUNT 10
#define SOF_IPC4_FW_CFG_MAX_MODULE_PIN_COUNT 11
#define SOF_IPC4_FW_CFG_MODULES_COUNT 12
#define SOF_IPC4_FW_CFG_MAX_MOD_INST_COUNT 13
#define SOF_IPC4_FW_CFG_MAX_LL_TASKS_PER_PRI_COUNT 14
#define SOF_IPC4_FW_CFG_LL_PRI_COUNT 15
#define SOF_IPC4_FW_CFG_MAX_DP_TASKS_COUNT 16
#define SOF_IPC4_FW_CFG_MAX_LIBS_COUNT 17
#define SOF_IPC4_FW_CFG_SCHEDULER_CONFIG 18
#define SOF_IPC4_FW_CFG_XTAL_FREQ_HZ 19
#define SOF_IPC4_FW_CFG_POWER_GATING_POLICY 22
#define SOF_IPC4_FW_CFG_ASSERT_MODE 23
#define SOF_IPC4_FW_CFG_CONTEXT_SAVE 29

/* HW_CONFIG tuple types */
#define SOF_IPC4_HW_CFG_VERSION 0
#define SOF_IPC4_HW_CFG_DSP_CORES 1
#define SOF_IPC4_HW_CFG_MEM_PAGE_BYTES 2
#define SOF_IPC4_HW_CFG_TOTAL_PHYS_MEM_PAGES 3
#define SOF_IPC4_HW_CFG_I2S_CAPS 4
#define SOF_IPC4_HW_CFG_GPDMA_CAPS 5
#define SOF_IPC4_HW_CFG_GATEWAY_COUNT 6
#define SOF_IPC4_HW_CFG_HP_EBB_COUNT 7
#define SOF_IPC4_HW_CFG_LP_EBB_COUNT 8
#define SOF_IPC4_HW_CFG_EBB_SIZE_BYTES 9
#define SOF_IPC4_HW_CFG_UAOL_CAPS 10
#define SOF_IPC4_HW_CFG_MIC_PRIVACY_CAPS 11

/* notifications */
#define SOF_IPC4_NOTIF_TYPE_SHIFT 16
#define SOF_IPC4_NOTIF_TYPE_MASK (0xffu << SOF_IPC4_NOTIF_TYPE_SHIFT)
#define SOF_IPC4_NOTIF_TYPE_GET(x) (((x) & SOF_IPC4_NOTIF_TYPE_MASK) >> SOF_IPC4_NOTIF_TYPE_SHIFT)
#define SOF_IPC4_NOTIFY_PHRASE_DETECTED 4
#define SOF_IPC4_NOTIFY_RESOURCE_EVENT 5
#define SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS 6
#define SOF_IPC4_NOTIFY_TIMESTAMP_CAPTURED 7
#define SOF_IPC4_NOTIFY_FW_READY 8
#define SOF_IPC4_NOTIFY_FW_AUD_CLASS_RESULT 9
#define SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT 10
#define SOF_IPC4_NOTIFY_MODULE_NOTIFICATION 12
#define SOF_IPC4_NOTIFY_PROBE_DATA_AVAILABLE 14
#define SOF_IPC4_NOTIFY_ASYNC_MSG_SRVC_MESSAGE 15
#define SOF_IPC4_NOTIFY_TYPE_LAST 16
#define SOF_IPC4_LOG_CORE_GET(x) (((x) >> 12) & 0xfu)
#define SOF_IPC4_FW_READY_LIB_RESTORED (1u << 15)

/* fixed window roles */
#define SOF_IPC4_WINDOW_INBOX 0 /* firmware registers + DSP-to-host box */
#define SOF_IPC4_WINDOW_OUTBOX 1 /* host-to-DSP box */
#define SOF_IPC4_WINDOW_DEBUG 2
#define SOF_IPC4_UPLINK_OFFSET 0x1000u /* inside window 0 */

/* debug window: page 0 = slot descriptors, then 15 slots of a page each */
#define SOF_IPC4_DEBUG_DESC_SIZE 12
#define SOF_IPC4_DEBUG_SLOTS 15
#define SOF_IPC4_DEBUG_SLOT_SIZE 0x1000u
#define SOF_IPC4_DEBUG_SLOT_CRITICAL_LOG 0x54524300u
#define SOF_IPC4_DEBUG_SLOT_DEBUG_LOG 0x474f4c00u
#define SOF_IPC4_DEBUG_SLOT_GDB_STUB 0x42444700u
#define SOF_IPC4_DEBUG_SLOT_TELEMETRY 0x4c455400u
#define SOF_IPC4_DEBUG_SLOT_BROKEN 0x44414544u
#define SOF_IPC4_DEBUG_SLOT_TYPE_MASK 0xffffff00u

/* ===================================================================== */
/* Firmware image (IPC4 .ri format)                                       */
/* ===================================================================== */

#define SOF_MAN4_EXT_MAGIC 0x31454124u /* "$AE1" */
#define SOF_MAN4_FW_MAGIC 0x314d4124u /* "$AM1" */
#define SOF_MAN4_FW_HDR_OFFSET 0x2000u /* after the extended manifest */
#define SOF_MAN4_MOD_NAME_LEN 8
#define SOF_MAN4_LIB_ID_SHIFT 12

struct sof_man4_ext_hdr {
	uint32_t id;
	uint32_t len;
	uint16_t version_major;
	uint16_t version_minor;
	uint32_t num_module_entries;
} __attribute__((packed));

struct sof_man4_fw_hdr {
	uint32_t id;
	uint32_t len;
	uint8_t name[8];
	uint32_t preload_page_count;
	uint32_t fw_image_flags;
	uint32_t feature_mask;
	uint16_t major_version;
	uint16_t minor_version;
	uint16_t hotfix_version;
	uint16_t build_version;
	uint32_t num_module_entries;
	uint32_t hw_buf_base_addr;
	uint32_t hw_buf_length;
	uint32_t load_offset;
} __attribute__((packed));

struct sof_man4_segment {
	uint32_t flags;
	uint32_t v_base_addr;
	uint32_t file_offset;
} __attribute__((packed));

struct sof_man4_module {
	uint32_t id;
	uint8_t name[SOF_MAN4_MOD_NAME_LEN];
	uint8_t uuid[16];
	uint32_t type;
	uint8_t hash[32];
	uint32_t entry_point;
	uint16_t cfg_offset;
	uint16_t cfg_count;
	uint32_t affinity_mask;
	uint16_t instance_max_count;
	uint16_t instance_stack_size;
	struct sof_man4_segment segments[3];
} __attribute__((packed));

struct sof_man4_module_config {
	uint32_t par[4];
	uint32_t is_bytes;
	uint32_t cps;
	uint32_t ibs;
	uint32_t obs;
	uint32_t module_flags;
	uint32_t cpc;
	uint32_t obls;
} __attribute__((packed));

/* ===================================================================== */
/* Driver state                                                           */
/* ===================================================================== */

#define SOF_DRV "sof"
#define SOF_MAX_STREAMS 30
#define SOF_BDL_BYTES 4096u
#define SOF_BDL_MAX_ENTRIES (SOF_BDL_BYTES / 16u)
#define SOF_MAX_LIBS 8
#define SOF_MAX_FW_MODULES 96 /* per library */
#define SOF_MAX_MOD_CFGS 64 /* per module, kept for CPC lookups */
#define SOF_MAX_SDW_CFG 16
#define SOF_BOOT_ATTEMPTS 3
#define SOF_IPC_TIMEOUT_US 500000u /* sleeping context */
#define SOF_IPC_ATOMIC_TIMEOUT_US 300000u /* interrupts off */
#define SOF_FW_READY_TIMEOUT_US 3000000u
#define SOF_ROM_INIT_TIMEOUT_US 300000u
#define SOF_ROM_IPC_DONE_TIMEOUT_US 500000u
#define SOF_BASEFW_TIMEOUT_US 3000000u
#define SOF_RESET_TIMEOUT_US 50000u
#define SOF_PD_TIMEOUT_US 50000u
#define SOF_CL_FORMAT 0x40 /* code loader stream: 8 bit, 1 channel */

/* IPC doorbell layouts */
#define SOF_IPC_LAYOUT_CAVS 0
#define SOF_IPC_LAYOUT_ACE 1

/* sof_chip.flags */
#define SOF_CHIP_DECLINE (1u << 0) /* recognised, no IPC4 firmware path */
#define SOF_CHIP_ICCMAX (1u << 1) /* run the ICCMAX capture during boot */
#define SOF_CHIP_ACE (1u << 2)
#define SOF_CHIP_ACE2 (1u << 3) /* SoundWire/SSP/DMIC behind ML alt links */
#define SOF_CHIP_ACE3 (1u << 4) /* second power control register */
#define SOF_CHIP_EXPERIMENTAL (1u << 5) /* unverified generation */

struct sof_chip {
	const char *name; /* "mtl" */
	int platform; /* SOF_PLAT_* */
	int cores;
	uint32_t init_core_mask;
	uint32_t host_managed_mask;
	int ipc_layout;
	uint32_t rom_status; /* FSR register in BAR4 */
	uint32_t rom_init_timeout_us;
	int ssp_count;
	uint32_t ssp_base;
	uint32_t d0i3c; /* BAR0 offset */
	uint32_t flags; /* SOF_CHIP_* */
	const char *generation; /* "ACE 1.0" */
};

struct sof_pci_id {
	uint16_t vendor, device;
	const struct sof_chip *chip;
	const char *name; /* "Arrow Lake-H" */
	const char *tag; /* "arl": firmware directory and display name */
	const char *fw_dir; /* "intel/sof-ipc4/arl" */
	const char *fw_file; /* "sof-arl.ri" */
	const char *lib_dir; /* "intel/sof-ipc4-lib/arl" */
	const char *decline_reason; /* NULL when supported */
};

/* One HDA DMA engine (stream descriptor) of the controller. */
struct sof_hstream {
	struct sof_host *host;
	int index; /* engine index (register order: inputs, outputs) */
	int direction; /* AUDIO_DIR_* */
	int tag; /* 1..15, per direction */
	int host_in_use; /* host side claimed */
	int link_in_use; /* link side (PPLC) claimed */
	uint32_t sd_off;
	uint8_t *bdl; /* SOF_BDL_BYTES, uncached */
	uint64_t bdl_phys;
	uint32_t nbdl;
	uint32_t buffer_bytes, period_bytes;
	uint16_t format;
	volatile int running;
	volatile int link_running;
	volatile uint32_t ioc_count; /* completions seen by the interrupt */
	uint32_t irq_count;
	uint32_t fifo_errors, desc_errors;
	struct snd_pcm_substream *substream; /* when it carries a PCM */
	void *owner;
	int spib_on;
	int l1_blocked; /* this stream holds DMI L1 off (cAVS capture) */
};

/* A firmware module from a manifest. */
struct sof_fw_module {
	uint32_t id; /* (library << 12) | index */
	char name[SOF_MAN4_MOD_NAME_LEN + 1];
	uint8_t uuid[16];
	uint32_t type;
	uint32_t affinity_mask;
	uint16_t instance_max;
	uint16_t stack_size;
	uint16_t ncfg;
	struct sof_man4_module_config *cfg; /* ncfg entries, owned */
};

/* The base firmware (id 0) or a loadable library. */
struct sof_fw_lib {
	int used;
	uint32_t id;
	char name[9];
	char file[128];
	uint16_t version[4];
	uint32_t payload_offset;
	uint32_t payload_bytes;
	int nmod;
	struct sof_fw_module *mod;
};

/* DMA memory in page-aligned pieces (the code loader's image). */
struct sof_dma_chunk {
	uint8_t *virt;
	uint64_t phys;
	uint32_t bytes; /* used */
	uint32_t alloc; /* allocated */
};
#define SOF_MAX_CHUNKS 256

/* SoundWire DAI configuration reported through sof_dai_sdw_config(). */
struct sof_sdw_cfg {
	int used;
	int link;
	int alh_stream;
	uint32_t rate, channels, bits;
	int pcm_idx, pcm_dir; /* the PCM being prepared when it came */
	struct sof_hstream *link_dma; /* ACE2+: the HDA link DMA it uses */
	uint16_t format;
};

/* A sleeping lock for process context; spins while the scheduler is off. */
struct sof_mutex {
	volatile int locked;
	spinlock_t lock;
};

/* Boot states */
#define SOF_BOOT_NONE 0
#define SOF_BOOT_PROBED 1
#define SOF_BOOT_LOADING 2
#define SOF_BOOT_FW_READY 3
#define SOF_BOOT_RUNNING 4 /* card registered */
#define SOF_BOOT_FAILED 5
#define SOF_BOOT_CRASHED 6

struct sof_host {
	struct sof_dev s; /* MUST stay first: see sof_host_of() */

	const struct sof_pci_id *id;
	const struct sof_chip *chip;
	char name[48]; /* "Arrow Lake-H 0000:00:1f.3" */
	char bus[32];
	uint32_t bar0_pages, bar4_pages;
	uint32_t bar0_map_size, bar4_map_size;

	/* HDA controller */
	uint16_t gcap;
	int num_iss, num_oss, num_bss;
	int nstreams;
	int addr64;
	uint32_t ppcap, spbcap, mlcap, gtscap, drsmcap; /* 0 = absent */
	struct sof_hstream st[SOF_MAX_STREAMS];
	uint32_t *posbuf;
	uint64_t posbuf_phys;
	spinlock_t reg_lock; /* INTCTL, PPCTL, SPBFCCTL, EM2 RMW */
	int chip_up;
	int capture_l1_blocks;

	/* multi-link */
	int nlinks;
	struct {
		uint32_t off; /* from BAR0 */
		uint32_t lcap;
		uint32_t leptr;
		int alt;
		int elid;
		int slcount;
		int ofls;
		int intc;
		uint32_t base; /* alternate link register block, from BAR0 */
	} link[SOF_ML_MAX_LINKS];

	/* ownership: the HDA driver holds the controller until we boot */
	int hda_release_pending;
	int taken; /* the controller is programmed by this driver */

	/* interrupts */
	int msi;
	int gsi;
	int irq_registered;
	uint32_t irq_count, irq_spurious;
	uint32_t irq_ipc, irq_stream, irq_sdw;

	/* DSP */
	int dsp_powered;
	int core0_running;
	int gating_off;
	int boot_state;
	int boot_error;
	int boot_attempt;
	uint32_t boot_rom_status;
	uint32_t boot_ms;
	char fw_path[128];

	/* IPC */
	uint32_t inbox_off; /* DSP-to-host payloads (BAR4 offset) */
	uint32_t outbox_off; /* host-to-DSP payloads */
	uint32_t fwregs_off;
	uint32_t debug_off;
	uint32_t dl_mbox_bytes, ul_mbox_bytes;
	volatile int ipc_live; /* the ROM handed the doorbells over */
	volatile int fw_ready;
	volatile uint32_t fw_ready_primary;
	volatile int tx_busy; /* a transaction owns the doorbell */
	volatile int tx_waiting_reply;
	volatile int tx_acked; /* DSP took our message (IDA done) */
	uint32_t tx_primary, tx_extension;
	uint8_t *rx_buf; /* SOF_IPC4_MAILBOX_SIZE, reply payload bounce */
	volatile uint32_t rx_len;
	uint32_t rx_want; /* bytes the waiter wants copied */
	struct wait_queue_head ipc_wq;
	uint32_t ipc_timeout_us; /* override for slow requests, 0 = default */
	uint32_t ipc_tx_count, ipc_err_count, ipc_timeouts;
	uint32_t notif_count[SOF_IPC4_NOTIFY_TYPE_LAST];
	uint32_t notif_unknown;
	uint32_t last_error_status;
	uint32_t last_error_primary;
	uint32_t underruns, overruns;
	volatile int crashed;
	int panic_dumped;
	uint32_t exception_count;

	/* firmware configuration (FW_CONFIG / HW_CONFIG) */
	uint32_t cfg_max_ppl, cfg_max_libs, cfg_modules, cfg_trace_bytes;
	uint32_t cfg_max_mod_inst, cfg_slow_clk, cfg_fast_clk, cfg_xtal;
	uint32_t cfg_alh_level, cfg_context_save, cfg_power_gating;
	uint32_t hw_cores, hw_page_bytes, hw_total_pages, hw_gateways;
	uint32_t hw_mic_privacy;

	/* firmware and libraries */
	struct sof_fw_lib lib[SOF_MAX_LIBS];
	struct sof_dma_chunk cl_chunk[SOF_MAX_CHUNKS];
	int cl_nchunks;

	/* SoundWire DAI configurations */
	struct sof_sdw_cfg sdwcfg[SOF_MAX_SDW_CFG];
	int prep_pcm, prep_dir; /* context for sof_dai_sdw_config */
	int sdw_up;

	/* card side */
	struct sof_mutex ctl_mutex;
	struct sof_mutex pcm_mutex;
	char tplg_files[6][96];
	int ntplg;
	char codec_desc[64];

	struct sof_host *next;
};

static inline struct sof_host *sof_host_of(struct sof_dev *sdev)
{
	return (struct sof_host *)sdev;
}

static inline int sof_is_ace(const struct sof_host *h)
{
	return h->chip && (h->chip->flags & SOF_CHIP_ACE);
}

static inline int sof_is_ace2(const struct sof_host *h)
{
	return h->chip && (h->chip->flags & SOF_CHIP_ACE2);
}

/* ---- register access ------------------------------------------------- */

static inline uint32_t sof_hda_r32(struct sof_host *h, uint32_t off)
{
	return *(volatile uint32_t *)(h->s.hda + off);
}
static inline uint16_t sof_hda_r16(struct sof_host *h, uint32_t off)
{
	return *(volatile uint16_t *)(h->s.hda + off);
}
static inline uint8_t sof_hda_r8(struct sof_host *h, uint32_t off)
{
	return *(volatile uint8_t *)(h->s.hda + off);
}
static inline void sof_hda_w32(struct sof_host *h, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(h->s.hda + off) = v;
}
static inline void sof_hda_w16(struct sof_host *h, uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)(h->s.hda + off) = v;
}
static inline void sof_hda_w8(struct sof_host *h, uint32_t off, uint8_t v)
{
	*(volatile uint8_t *)(h->s.hda + off) = v;
}
static inline void sof_hda_upd32(struct sof_host *h, uint32_t off,
				 uint32_t mask, uint32_t val)
{
	uint32_t v = sof_hda_r32(h, off);
	sof_hda_w32(h, off, (v & ~mask) | (val & mask));
}
static inline void sof_hda_upd8(struct sof_host *h, uint32_t off, uint8_t mask,
				uint8_t val)
{
	uint8_t v = sof_hda_r8(h, off);
	sof_hda_w8(h, off, (uint8_t)((v & ~mask) | (val & mask)));
}
static inline uint32_t sof_dsp_r32(struct sof_host *h, uint32_t off)
{
	return *(volatile uint32_t *)(h->s.dsp + off);
}
static inline void sof_dsp_w32(struct sof_host *h, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(h->s.dsp + off) = v;
}
static inline void sof_dsp_upd32(struct sof_host *h, uint32_t off,
				 uint32_t mask, uint32_t val)
{
	uint32_t v = sof_dsp_r32(h, off);
	sof_dsp_w32(h, off, (v & ~mask) | (val & mask));
}

/* ---- time, waiting, locking (sof_dsp.c) ------------------------------- */

/* Set by sof_late_init(): the scheduler runs and waits may sleep. */
extern int g_sof_can_block;


uint64_t sof_now_us(void);
int sof_can_sleep(void);
/* Sleep (when possible) or busy-wait for `us' microseconds. */
void sof_wait_us(uint32_t us);
/* Poll a register until (value & mask) == want.  Returns 0 or -ETIMEDOUT;
 * the last value read is stored in *last when non-NULL. */
int sof_poll_hda32(struct sof_host *h, uint32_t off, uint32_t mask,
		   uint32_t want, uint32_t timeout_us, uint32_t *last);
int sof_poll_dsp32(struct sof_host *h, uint32_t off, uint32_t mask,
		   uint32_t want, uint32_t timeout_us, uint32_t *last);
void sof_mutex_init(struct sof_mutex *m, const char *name);
void sof_mutex_lock(struct sof_mutex *m);
void sof_mutex_unlock(struct sof_mutex *m);

/* ---- controller, links, DSP power (sof_dsp.c) ------------------------- */

int sof_hda_enum_caps(struct sof_host *h);
int sof_hda_link_reset(struct sof_host *h, int enter);
int sof_hda_init_chip(struct sof_host *h);
void sof_hda_stop_chip(struct sof_host *h);
void sof_ml_enum(struct sof_host *h);
int sof_ml_power_hda_links(struct sof_host *h, int on);
void sof_ml_set_offload(struct sof_host *h, int on);
int sof_ml_find_alt(struct sof_host *h, int elid);
int sof_ml_sdw_irq_pending(struct sof_host *h);
void sof_pp_enable(struct sof_host *h, int on);
void sof_pp_int_enable(struct sof_host *h, int on);
void sof_dsp_gating(struct sof_host *h, int enable);
int sof_dsp_power_up(struct sof_host *h);
int sof_dsp_power_down(struct sof_host *h);
int sof_cavs_core_power_up(struct sof_host *h, uint32_t mask);
int sof_cavs_core_run(struct sof_host *h, uint32_t mask);
int sof_cavs_core_reset_power_down(struct sof_host *h, uint32_t mask);
int sof_cavs_core_is_enabled(struct sof_host *h, uint32_t mask);
int sof_ace_core0_power(struct sof_host *h, int on);
void sof_cavs_ssp_set_consumer(struct sof_host *h);
void sof_dsp_ipc_int_enable(struct sof_host *h, int on);
int sof_dsp_irq_enable(struct sof_host *h, int on);
void sof_dsp_sdw_int_enable(struct sof_host *h, int on);
int sof_dsp_ipc_irq_pending(struct sof_host *h);
int sof_dsp_sdw_irq_pending(struct sof_host *h);
uint32_t sof_dsp_rom_status(struct sof_host *h);
const char *sof_fsr_state_name(struct sof_host *h, uint32_t fsr);
const char *sof_rom_error_name(uint32_t code);
void sof_dsp_dump(struct sof_host *h, const char *why);
void sof_dsp_panic_dump(struct sof_host *h);
long sof_dsp_diag(struct sof_host *h, char *buf, size_t len);

/* ---- HDA DMA engines (sof_dsp.c) -------------------------------------- */

int sof_streams_init(struct sof_host *h);
void sof_streams_free(struct sof_host *h);
struct sof_hstream *sof_stream_get(struct sof_host *h, int direction);
void sof_stream_put(struct sof_hstream *st);
struct sof_hstream *sof_link_stream_get(struct sof_host *h, int direction);
void sof_link_stream_put(struct sof_hstream *st);
int sof_stream_reset(struct sof_hstream *st);
/* Program a stopped engine for a ring of `nchunks' pieces; IOC on every
 * `period_bytes' boundary (0 = only at the end).  `decouple' sets PROCEN. */
int sof_stream_program(struct sof_hstream *st,
		       const struct sof_dma_chunk *chunk, int nchunks,
		       uint32_t total_bytes, uint32_t period_bytes,
		       uint16_t format, int decouple);
void sof_stream_spib(struct sof_hstream *st, int enable, uint32_t value);
int sof_stream_start(struct sof_hstream *st);
int sof_stream_stop(struct sof_hstream *st);
void sof_stream_release_hw(struct sof_hstream *st);
uint32_t sof_stream_position(struct sof_hstream *st);
int sof_link_dma_setup(struct sof_hstream *st, uint16_t format);
int sof_link_dma_run(struct sof_hstream *st, int run);
void sof_link_losidv(struct sof_host *h, int link, int tag, int on);
uint16_t sof_hda_format(uint32_t rate, uint32_t channels, uint32_t bits);
/* Interrupt-side service of the stream status bits in INTSTS. */
void sof_streams_irq(struct sof_host *h, uint32_t intsts);

/* ---- IPC4 (sof_ipc4.c) ----------------------------------------------- */

void sof_ipc4_init_host(struct sof_host *h);
void sof_ipc4_reset(struct sof_host *h);
/* Service the doorbells (interrupt handler and pollers).  Returns 1 when
 * something was handled. */
int sof_ipc4_service(struct sof_host *h);
int sof_ipc4_status_errno(uint32_t status);
const char *sof_ipc4_status_text(uint32_t status);
const char *sof_ipc4_notif_name(uint32_t type);
int sof_ipc4_large_config(struct sof_host *h, uint32_t module_id,
			  uint32_t instance, uint32_t param, void *data,
			  size_t len, int set, size_t *out_len);
int sof_ipc4_query_fw_config(struct sof_host *h);
int sof_ipc4_set_pipeline_state(struct sof_host *h, uint32_t pipe,
				uint32_t state);
int sof_ipc4_glb(struct sof_host *h, uint32_t type, uint32_t primary_bits,
		 uint32_t extension, const void *data, size_t len);
int sof_ipc4_set_dx(struct sof_host *h, uint32_t core_mask, uint32_t dx_mask);
void sof_ipc4_dump_regs(struct sof_host *h);
long sof_ipc4_diag(struct sof_host *h, char *buf, size_t len);

/* ---- firmware (sof_loader.c) ------------------------------------------ */

int sof_fw_boot(struct sof_host *h);
/* 0 when a base firmware file for this device can be read. */
int sof_fw_probe_file(struct sof_host *h);
void sof_fw_release(struct sof_host *h);
int sof_fw_load_library(struct sof_host *h, const char *path, int optional);
int sof_fw_load_library_uuid(struct sof_host *h, const uint8_t uuid[16]);
/* Look a module up by UUID across the loaded images, loading the library
 * named after the UUID when it is missing.  0 and the module or -ENOENT. */
int sof_fw_find_module(struct sof_dev *sdev, const uint8_t uuid[16],
		       const struct sof_fw_module **out);
long sof_fw_diag(struct sof_host *h, char *buf, size_t len);
void sof_uuid_str(const uint8_t uuid[16], char *out, size_t len);

/* ---- card (sof_pcm.c) ------------------------------------------------ */

/* The DSP half of sof_dai_sdw_config(): records the SoundWire stream for
 * the PCM being prepared and, on ACE 2.x+, sets up the HDA link DMA and
 * the link's PCM stream channel map.  0 or -errno. */
int sof_dsp_sdw_dai_config(struct sof_dev *sdev, int link, int alh_stream,
			   uint32_t rate, uint32_t channels, uint32_t bits);

int sof_card_create(struct sof_host *h);
void sof_card_destroy(struct sof_host *h);
/* Start/stop the link DMAs that SoundWire DAI configurations attached to
 * PCM `idx' direction `dir' (ACE2+). */
void sof_pcm_link_dmas(struct sof_host *h, int idx, int dir, int run);
void sof_pcm_link_dmas_release(struct sof_host *h, int idx, int dir);
long sof_pcm_diag(struct sof_host *h, char *buf, size_t len);

/* ---- small helpers --------------------------------------------------- */

/* Append formatted text to buf at *pos (bounded).  Returns the new pos. */
size_t sof_append(char *buf, size_t len, size_t pos, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
void sof_strlcpy(char *dst, const char *src, size_t n);

#define sof_log(h, fmt, ...) \
	kprintf("[snd] sof: %s: " fmt, (h)->id ? (h)->id->tag : "?", ##__VA_ARGS__)

#endif /* KERNEL_DEV_SOUND_SOF_REGS_H */
