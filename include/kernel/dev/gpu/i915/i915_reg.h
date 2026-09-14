// LikeOS-64 -- register offsets of Intel integrated graphics (Gen8 onwards).
//
// Offsets are into the MMIO window (BAR0); names follow the datasheets'
// register names.  Only what the driver uses is here; the file grows with
// the driver.  Generation-specific offsets carry the generation in their
// name (GEN8_, GEN9_, GEN11_, GEN12_).
#ifndef KERNEL_DEV_GPU_I915_REG_H
#define KERNEL_DEV_GPU_I915_REG_H

/* A "masked" register: the upper 16 bits say which of the lower 16 the
 * write changes.  Writing only the bits you mean is how most GT control
 * registers are updated. */
#define I915_MASKED_ENABLE(bit) (((bit) << 16) | (bit))
#define I915_MASKED_DISABLE(bit) ((bit) << 16)

/* ---- PCI config registers of the graphics function ---------------------- */
#define I915_PCI_GGC 0x50 /* graphics control: stolen + GTT sizes */
#define I915_PCI_BSM 0x5C /* base of stolen memory (data) */
#define I915_PCI_BGSM 0xB0 /* base of GTT stolen memory (Gen8-9) */
#define I915_PCI_ASLS 0xFC /* ACPI OpRegion base */
#define I915_PCI_MSAC 0x62 /* aperture size (Gen<8) */
#define I915_PCI_SWSCI 0xE8

#define GGC_GMS_SHIFT 8
#define GGC_GMS_MASK 0xFF /* stolen memory, Gen8+: see i915_gtt.c */
#define GGC_GGMS_SHIFT 6
#define GGC_GGMS_MASK 0x3 /* GTT size: 1=2MB 2=4MB 3=8MB */

/* ---- identification / fuses --------------------------------------------- */
#define GEN8_FUSE2 0x9120
#define GEN8_F2_SS_DIS_SHIFT 21
#define GEN8_F2_SS_DIS_MASK (0x7 << GEN8_F2_SS_DIS_SHIFT)
#define GEN8_F2_S_ENA_SHIFT 25
#define GEN8_F2_S_ENA_MASK (0x7 << GEN8_F2_S_ENA_SHIFT)
#define GEN9_F2_SS_DIS_SHIFT 20
#define GEN9_F2_SS_DIS_MASK (0xf << GEN9_F2_SS_DIS_SHIFT)
#define GEN8_EU_DISABLE0 0x9134
#define GEN8_EU_DISABLE1 0x9138
#define GEN8_EU_DISABLE2 0x913c
#define GEN9_EU_DISABLE(slice) (0x9134 + (slice) * 0x4)
#define GEN10_MIRROR_FUSE3 0x9118
#define GEN10_L3BANK_PAIR_COUNT 4
#define GEN10_L3BANK_MASK 0x0F
#define GEN11_GT_VEBOX_VDBOX_DISABLE 0x9140
#define GEN11_GT_VDBOX_DISABLE_MASK 0xff
#define GEN11_GT_VEBOX_DISABLE_SHIFT 16
#define GEN11_GT_VEBOX_DISABLE_MASK (0x0f << GEN11_GT_VEBOX_DISABLE_SHIFT)
#define GEN11_GT_SLICE_ENABLE 0x9138
#define GEN11_GT_S_ENA_MASK 0xFF
#define GEN11_GT_SUBSLICE_DISABLE 0x913C
#define GEN12_GT_GEOMETRY_DSS_ENABLE 0x913C
#define GEN12_GT_COMPUTE_DSS_ENABLE 0x9144
#define GEN11_EU_DISABLE 0x9134
#define GEN11_EU_DIS_MASK 0xFF
#define GEN12_EU_DISABLE 0x9134

#define GEN6_GDRST 0x941c
#define GEN6_GRDOM_FULL (1 << 0)
#define GEN6_GRDOM_RENDER (1 << 1)
#define GEN6_GRDOM_MEDIA (1 << 2)
#define GEN6_GRDOM_BLT (1 << 3)
#define GEN6_GRDOM_VECS (1 << 4)
#define GEN9_GRDOM_GUC (1 << 5)
#define GEN8_GRDOM_MEDIA2 (1 << 7)

/* Timestamp clock */
#define CTC_MODE 0xa26c
#define CTC_SOURCE_DIVIDE_LOGIC (1 << 0)
#define CTC_SHIFT_PARAMETER_SHIFT 1
#define CTC_SHIFT_PARAMETER_MASK (0x3 << CTC_SHIFT_PARAMETER_SHIFT)
#define RPM_CONFIG0 0x0d00
#define GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT 3
#define GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK (1 << 3)
#define GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT 3
#define GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK (0x7 << 3)
#define GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT 1
#define GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK (0x3 << 1)

/* ---- forcewake ----------------------------------------------------------- */
#define FORCEWAKE_KERNEL (1 << 0)
#define FORCEWAKE_MT 0xa188 /* Gen8: one multi-thread domain */
#define FORCEWAKE_ACK_HSW 0x130044
#define FORCEWAKE_RENDER_GEN9 0xa278
#define FORCEWAKE_ACK_RENDER_GEN9 0x0d84
#define FORCEWAKE_GT_GEN9 0xa188 /* "blitter" on Gen9, "GT" on Gen11+ */
#define FORCEWAKE_ACK_GT_GEN9 0x130044
#define FORCEWAKE_MEDIA_GEN9 0xa270
#define FORCEWAKE_ACK_MEDIA_GEN9 0x0d88
#define FORCEWAKE_MEDIA_VDBOX_GEN11(n) (0xa540 + (n) * 4)
#define FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(n) (0x0d50 + (n) * 4)
#define FORCEWAKE_MEDIA_VEBOX_GEN11(n) (0xa560 + (n) * 4)
#define FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(n) (0x0d70 + (n) * 4)
#define FORCEWAKE_GT_GEN12 0xa188
#define FORCEWAKE_ACK_GT_MTL 0x0d84 /* Meteor Lake moved the GT ack */
#define FPGA_DBG 0x42300
#define FPGA_DBG_RM_NOCLAIM (1 << 31)

/* ---- interrupts: Gen8-10 -------------------------------------------------- */
#define GEN8_MASTER_IRQ 0x44200
#define GEN8_MASTER_IRQ_CONTROL (1u << 31)
#define GEN8_PCU_IRQ (1 << 30)
#define GEN8_DE_PCH_IRQ (1 << 23)
#define GEN8_DE_MISC_IRQ (1 << 22)
#define GEN8_DE_PORT_IRQ (1 << 20)
#define GEN8_DE_PIPE_C_IRQ (1 << 18)
#define GEN8_DE_PIPE_B_IRQ (1 << 17)
#define GEN8_DE_PIPE_A_IRQ (1 << 16)
#define GEN8_DE_PIPE_IRQ(pipe) (1 << (16 + (pipe)))
#define GEN8_GT_VECS_IRQ (1 << 6)
#define GEN8_GT_GUC_IRQ (1 << 5)
#define GEN8_GT_PM_IRQ (1 << 4)
#define GEN8_GT_VCS1_IRQ (1 << 3)
#define GEN8_GT_VCS0_IRQ (1 << 2)
#define GEN8_GT_BCS_IRQ (1 << 1)
#define GEN8_GT_RCS_IRQ (1 << 0)

#define GEN8_GT_ISR(n) (0x44300 + (n) * 0x10)
#define GEN8_GT_IMR(n) (0x44304 + (n) * 0x10)
#define GEN8_GT_IIR(n) (0x44308 + (n) * 0x10)
#define GEN8_GT_IER(n) (0x4430c + (n) * 0x10)
/* GT0: RCS bits 0-15, BCS bits 16-31; GT1: VCS0 0-15, VCS1 16-31;
 * GT2: PM; GT3: VECS 0-15, GuC 16-31 */
#define GEN8_RCS_IRQ_SHIFT 0
#define GEN8_BCS_IRQ_SHIFT 16
#define GEN8_VCS0_IRQ_SHIFT 0
#define GEN8_VCS1_IRQ_SHIFT 16
#define GEN8_VECS_IRQ_SHIFT 0
#define GT_RENDER_USER_INTERRUPT (1 << 0)
#define GT_CONTEXT_SWITCH_INTERRUPT (1 << 8)
#define GT_RENDER_CS_MASTER_ERROR_INTERRUPT (1 << 3)
#define GT_WAIT_SEMAPHORE_INTERRUPT (1 << 11)

#define GEN8_DE_PIPE_ISR(pipe) (0x44400 + (pipe) * 0x10)
#define GEN8_DE_PIPE_IMR(pipe) (0x44404 + (pipe) * 0x10)
#define GEN8_DE_PIPE_IIR(pipe) (0x44408 + (pipe) * 0x10)
#define GEN8_DE_PIPE_IER(pipe) (0x4440c + (pipe) * 0x10)
#define GEN8_PIPE_FIFO_UNDERRUN (1u << 31)
#define GEN8_PIPE_CDCLK_CRC_ERROR (1 << 29)
#define GEN8_PIPE_CDCLK_CRC_DONE (1 << 28)
#define GEN9_PIPE_PLANE_FLIP_DONE(p) (1 << (3 + (p))) /* planes 1..4 */
#define GEN8_PIPE_CURSOR_FAULT (1 << 10)
#define GEN8_PIPE_PRIMARY_FLIP_DONE (1 << 4)
#define GEN8_PIPE_SCAN_LINE_EVENT (1 << 2)
#define GEN8_PIPE_VSYNC (1 << 1)
#define GEN8_PIPE_VBLANK (1 << 0)

#define GEN8_DE_PORT_ISR 0x44440
#define GEN8_DE_PORT_IMR 0x44444
#define GEN8_DE_PORT_IIR 0x44448
#define GEN8_DE_PORT_IER 0x4444c
#define GEN9_AUX_CHANNEL_D (1 << 27)
#define GEN9_AUX_CHANNEL_C (1 << 26)
#define GEN9_AUX_CHANNEL_B (1 << 25)
#define GEN8_AUX_CHANNEL_A (1 << 0)
#define GEN8_PORT_DP_A_HOTPLUG (1 << 3)
#define BXT_DE_PORT_HOTPLUG_MASK 0x7
#define BXT_DE_PORT_GMBUS (1 << 1)

#define GEN8_DE_MISC_ISR 0x44460
#define GEN8_DE_MISC_IMR 0x44464
#define GEN8_DE_MISC_IIR 0x44468
#define GEN8_DE_MISC_IER 0x4446c
#define GEN8_DE_MISC_GSE (1 << 27)
#define GEN8_DE_EDP_PSR (1 << 19)

#define GEN8_PCU_ISR 0x444e0
#define GEN8_PCU_IMR 0x444e4
#define GEN8_PCU_IIR 0x444e8
#define GEN8_PCU_IER 0x444ec

/* south (PCH) display interrupts */
#define SDEISR 0xc4000
#define SDEIMR 0xc4004
#define SDEIIR 0xc4008
#define SDEIER 0xc400c
#define SDE_GMBUS_CPT (1 << 17)
#define SDE_PORTA_HOTPLUG_SPT (1 << 24)
#define SDE_PORTE_HOTPLUG_SPT (1 << 25)
#define SDE_PORTD_HOTPLUG_CPT (1 << 23)
#define SDE_PORTC_HOTPLUG_CPT (1 << 22)
#define SDE_PORTB_HOTPLUG_CPT (1 << 21)
#define SDE_HOTPLUG_MASK_SPT (SDE_PORTE_HOTPLUG_SPT | SDE_PORTD_HOTPLUG_CPT | \
			      SDE_PORTC_HOTPLUG_CPT | SDE_PORTB_HOTPLUG_CPT | \
			      SDE_PORTA_HOTPLUG_SPT)

/* ---- interrupts: Gen11+ ------------------------------------------------- */
#define GEN11_GFX_MSTR_IRQ 0x190010
#define GEN11_MASTER_IRQ (1u << 31)
#define GEN11_DISPLAY_IRQ (1 << 16)
#define GEN11_GU_MISC_IRQ (1 << 29)
#define GEN11_GT_DW_IRQ(x) (1 << (x))
#define GEN11_GT_INTR_DW(x) (0x190018 + (x) * 4)
#define GEN11_IIR_REG_SELECTOR(x) (0x190070 + (x) * 4)
#define GEN11_INTR_IDENTITY_REG(x) (0x190060 + (x) * 4)
#define GEN11_INTR_DATA_VALID (1u << 31)
#define GEN11_INTR_ENGINE_CLASS(x) (((x) & 0x70000) >> 16)
#define GEN11_INTR_ENGINE_INSTANCE(x) (((x) & 0x3f00000) >> 20)
#define GEN11_INTR_ENGINE_INTR(x) ((x) & 0xffff)
#define GEN11_RENDER_COPY_INTR_ENABLE 0x190030
#define GEN11_VCS_VECS_INTR_ENABLE 0x190034
#define GEN11_GUC_SG_INTR_ENABLE 0x190038
#define GEN11_GPM_WGBOXPERF_INTR_ENABLE 0x19003c
#define GEN11_CRYPTO_RSVD_INTR_ENABLE 0x190040
#define GEN11_GUNIT_CSME_INTR_ENABLE 0x190044
#define GEN11_RCS0_RSVD_INTR_MASK 0x190090
#define GEN11_BCS_RSVD_INTR_MASK 0x1900a0
#define GEN11_VCS0_VCS1_INTR_MASK 0x1900a8
#define GEN11_VCS2_VCS3_INTR_MASK 0x1900ac
#define GEN11_VECS0_VECS1_INTR_MASK 0x1900d0
#define GEN11_GUC_SG_INTR_MASK 0x1900e8
#define GEN11_GPM_WGBOXPERF_INTR_MASK 0x1900ec
#define GEN11_CRYPTO_RSVD_INTR_MASK 0x1900f0
#define GEN11_GUNIT_CSME_INTR_MASK 0x1900f4
#define GEN12_CCS0_CCS1_INTR_MASK 0x190100
#define GEN12_CCS2_CCS3_INTR_MASK 0x190104
#define GEN11_DISPLAY_INT_CTL 0x44200
#define GEN11_DISPLAY_IRQ_ENABLE (1u << 31)
#define GEN11_AUDIO_CODEC_IRQ (1 << 24)
#define GEN11_DE_PCH_IRQ (1 << 23)
#define GEN11_DE_MISC_IRQ (1 << 22)
#define GEN11_DE_HPD_IRQ (1 << 21)
#define GEN11_DE_PORT_IRQ (1 << 20)
#define GEN11_DE_PIPE_D (1 << 19)
#define GEN11_DE_PIPE_C (1 << 18)
#define GEN11_DE_PIPE_B (1 << 17)
#define GEN11_DE_PIPE_A (1 << 16)
#define GEN11_DE_HPD_ISR 0x44470
#define GEN11_DE_HPD_IMR 0x44474
#define GEN11_DE_HPD_IIR 0x44478
#define GEN11_DE_HPD_IER 0x4447c
#define GEN11_GU_MISC_ISR 0x444f0
#define GEN11_GU_MISC_IMR 0x444f4
#define GEN11_GU_MISC_IIR 0x444f8
#define GEN11_GU_MISC_IER 0x444fc
#define GEN11_GU_MISC_GSE (1 << 27)

/* ---- display: what the firmware left on ------------------------------- */
#define PIPE_A 0
#define PIPE_B 1
#define PIPE_C 2
#define PIPE_D 3
#define TRANSCODER_EDP 3

#define PIPECONF(pipe) (0x70008 + (pipe) * 0x1000)
#define PIPECONF_ENABLE (1u << 31)
#define PIPECONF_STATE_ENABLE (1 << 30)
#define PIPESRC(pipe) (0x6001c + (pipe) * 0x1000)
#define PLANE_CTL(pipe, plane) (0x70180 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_CTL_ENABLE (1u << 31)
#define PLANE_CTL_FORMAT_MASK (0xf << 24)
#define PLANE_CTL_TILED_MASK (0x7 << 10)
#define PLANE_STRIDE(pipe, plane) (0x70188 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_SIZE(pipe, plane) (0x70190 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_SURF(pipe, plane) (0x7019c + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_OFFSET(pipe, plane) (0x701a4 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_POS(pipe, plane) (0x7018c + (pipe) * 0x1000 + (plane) * 0x100)

/* ---- GTT ----------------------------------------------------------------- */
#define GFX_FLSH_CNTL_GEN6 0x101008
#define GFX_FLSH_CNTL_EN (1 << 0)
#define GEN8_PRIVATE_PAT_LO 0x40e0
#define GEN8_PRIVATE_PAT_HI 0x40e4
#define GEN12_PAT_INDEX(n) (0x4800 + (n) * 4)

#endif

/* ======================================================================
 * Display engine, generation 9 (Skylake and its derivatives) onwards
 * ====================================================================== */

/* ---- power wells --------------------------------------------------------- */
#define HSW_PWR_WELL_CTL1 0x45400 /* BIOS */
#define HSW_PWR_WELL_CTL2 0x45404 /* driver */
#define HSW_PWR_WELL_CTL3 0x45408 /* KVMR */
#define HSW_PWR_WELL_CTL4 0x4540C /* debug */
/* Each well has a request bit (2*idx+1) and a state bit (2*idx). */
#define HSW_PWR_WELL_CTL_REQ(idx) (1u << ((idx) * 2 + 1))
#define HSW_PWR_WELL_CTL_STATE(idx) (1u << ((idx) * 2))
#define SKL_PW_CTL_IDX_PW_2 15
#define SKL_PW_CTL_IDX_PW_1 14
#define SKL_PW_CTL_IDX_DDI_D 4
#define SKL_PW_CTL_IDX_DDI_C 3
#define SKL_PW_CTL_IDX_DDI_B 2
#define SKL_PW_CTL_IDX_DDI_A_E 1
#define SKL_PW_CTL_IDX_MISC_IO 0
#define SKL_FUSE_STATUS 0x42000
#define SKL_FUSE_DOWNLOAD_STATUS (1u << 31)
#define SKL_FUSE_PG_DIST_STATUS(pg) (1u << (27 - (pg)))
#define GEN9_CLKGATE_DIS_0 0x46530
#define GEN9_CLKGATE_DIS_4 0x4653C
#define DC_STATE_DEBUG 0x45520
#define DC_STATE_DEBUG_MASK_CORES (1 << 0)
#define DC_STATE_DEBUG_MASK_MEMORY_UP (1 << 1)
#define DC_STATE_EN 0x45504
#define DC_STATE_EN_UPTO_DC5 (1 << 0)
#define DC_STATE_EN_DC9 (1 << 3)
#define DC_STATE_EN_UPTO_DC6 (2 << 0)
#define DC_STATE_EN_UPTO_DC5_DC6_MASK 0x3

/* ---- CDCLK and the PLLs (Skylake) ------------------------------------------ */
#define CDCLK_CTL 0x46000
#define CDCLK_FREQ_SEL_MASK (3 << 26)
#define CDCLK_FREQ_450_432 (0 << 26)
#define CDCLK_FREQ_540 (1 << 26)
#define CDCLK_FREQ_337_308 (2 << 26)
#define CDCLK_FREQ_675_617 (3 << 26)
#define CDCLK_FREQ_DECIMAL_MASK 0x7ff
#define LCPLL1_CTL 0x46010 /* DPLL0 */
#define LCPLL2_CTL 0x46014 /* DPLL1 */
#define WRPLL_CTL1 0x46040 /* DPLL2 */
#define WRPLL_CTL2 0x46060 /* DPLL3 */
#define LCPLL_PLL_ENABLE (1u << 31)
#define LCPLL_PLL_LOCK (1 << 30)
#define WRPLL_PLL_ENABLE (1u << 31)
#define DPLL_CTRL1 0x6C058
#define DPLL_CTRL1_OVERRIDE(id) (1 << ((id) * 6))
#define DPLL_CTRL1_SSC(id) (1 << ((id) * 6 + 4))
#define DPLL_CTRL1_HDMI_MODE(id) (1 << ((id) * 6 + 5))
#define DPLL_CTRL1_LINK_RATE_MASK(id) (7 << ((id) * 6 + 1))
#define DPLL_CTRL1_LINK_RATE_SHIFT(id) ((id) * 6 + 1)
#define DPLL_CTRL1_LINK_RATE(rate, id) ((rate) << ((id) * 6 + 1))
#define DPLL_CTRL1_LINK_RATE_2700 0
#define DPLL_CTRL1_LINK_RATE_1350 1
#define DPLL_CTRL1_LINK_RATE_810 2
#define DPLL_CTRL1_LINK_RATE_1620 3
#define DPLL_CTRL1_LINK_RATE_1080 4
#define DPLL_CTRL1_LINK_RATE_2160 5
#define DPLL_CTRL2 0x6C05C
#define DPLL_CTRL2_DDI_CLK_OFF(port) (1 << ((port) + 15))
#define DPLL_CTRL2_DDI_CLK_SEL_MASK(port) (3 << ((port) * 3 + 1))
#define DPLL_CTRL2_DDI_CLK_SEL_SHIFT(port) ((port) * 3 + 1)
#define DPLL_CTRL2_DDI_CLK_SEL(clk, port) ((clk) << ((port) * 3 + 1))
#define DPLL_CTRL2_DDI_SEL_OVERRIDE(port) (1 << ((port) * 3))
#define DPLL_STATUS 0x6C060
#define DPLL_LOCK(id) (1 << ((id) * 8))
#define DPLL_CFGCR1(id) (0x6C040 + ((id) - 1) * 8)
#define DPLL_CFGCR2(id) (0x6C044 + ((id) - 1) * 8)
#define DPLL_CFGCR1_FREQ_ENABLE (1u << 31)
#define DPLL_CFGCR1_DCO_FRACTION_MASK (0x7fff << 9)
#define DPLL_CFGCR1_DCO_FRACTION(x) ((x) << 9)
#define DPLL_CFGCR1_DCO_INTEGER_MASK (0x1ff)
#define DPLL_CFGCR2_QDIV_RATIO_MASK (0xff << 8)
#define DPLL_CFGCR2_QDIV_RATIO(x) ((x) << 8)
#define DPLL_CFGCR2_QDIV_MODE(x) ((x) << 7)
#define DPLL_CFGCR2_KDIV_MASK (3 << 5)
#define DPLL_CFGCR2_KDIV(x) ((x) << 5)
#define DPLL_CFGCR2_PDIV_MASK (7 << 2)
#define DPLL_CFGCR2_PDIV(x) ((x) << 2)
#define DPLL_CFGCR2_CENTRAL_FREQ_MASK (3)

/* pcode mailbox */
#define GEN6_PCODE_MAILBOX 0x138124
#define GEN6_PCODE_READY (1u << 31)
#define GEN6_PCODE_ERROR_MASK 0xFF
#define GEN6_PCODE_DATA 0x138128
#define GEN6_PCODE_DATA1 0x13812C
#define GEN9_PCODE_READ_MEM_LATENCY 0x6
#define GEN9_MEM_LATENCY_LEVEL_MASK 0xFF
#define GEN9_MEM_LATENCY_LEVEL_1_5_SHIFT 8
#define GEN9_MEM_LATENCY_LEVEL_2_6_SHIFT 16
#define GEN9_MEM_LATENCY_LEVEL_3_7_SHIFT 24
#define SKL_PCODE_CDCLK_CONTROL 0x7
#define SKL_CDCLK_PREPARE_FOR_CHANGE 0x3
#define SKL_CDCLK_READY_FOR_CHANGE 0x1

/* ---- DDI ------------------------------------------------------------------- */
#define PORT_A 0
#define PORT_B 1
#define PORT_C 2
#define PORT_D 3
#define PORT_E 4
#define DDI_BUF_CTL(port) (0x64000 + (port) * 0x100)
#define DDI_BUF_CTL_ENABLE (1u << 31)
#define DDI_BUF_TRANS_SELECT(n) ((n) << 24)
#define DDI_BUF_EMP_MASK (0xf << 24)
#define DDI_PORT_WIDTH(width) (((width) - 1) << 1)
#define DDI_PORT_WIDTH_MASK (7 << 1)
#define DDI_BUF_PORT_REVERSAL (1 << 16)
#define DDI_BUF_IS_IDLE (1 << 7)
#define DDI_A_4_LANES (1 << 4)
#define DDI_INIT_DISPLAY_DETECTED (1 << 0)
#define DDI_BUF_TRANS_LO(port, i) (0x64E00 + (port) * 0x60 + (i) * 8)
#define DDI_BUF_TRANS_HI(port, i) (0x64E04 + (port) * 0x60 + (i) * 8)
#define DDI_BUF_BALANCE_LEG_ENABLE (1u << 31)
#define DP_TP_CTL(port) (0x64040 + (port) * 0x100)
#define DP_TP_CTL_ENABLE (1u << 31)
#define DP_TP_CTL_MODE_SST (0 << 27)
#define DP_TP_CTL_MODE_MST (1 << 27)
#define DP_TP_CTL_FORCE_ACT (1 << 25)
#define DP_TP_CTL_ENHANCED_FRAME_ENABLE (1 << 18)
#define DP_TP_CTL_FDI_AUTOTRAIN (1 << 15)
#define DP_TP_CTL_LINK_TRAIN_MASK (7 << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT1 (0 << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT2 (1 << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT3 (4 << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT4 (5 << 8)
#define DP_TP_CTL_LINK_TRAIN_IDLE (2 << 8)
#define DP_TP_CTL_LINK_TRAIN_NORMAL (3 << 8)
#define DP_TP_CTL_SCRAMBLE_DISABLE (1 << 7)
#define DP_TP_STATUS(port) (0x64044 + (port) * 0x100)
#define DP_TP_STATUS_IDLE_DONE (1 << 25)
#define DP_TP_STATUS_ACT_SENT (1 << 24)
#define DP_TP_STATUS_MODE_STATUS_MST (1 << 23)
#define DP_TP_STATUS_AUTOTRAIN_DONE (1 << 12)
#define DDI_CLK_SEL(port) (0x46100 + (port) * 4)

/* AUX channels (Skylake: all on the north side) */
#define DP_AUX_CH_CTL(port) (0x64010 + (port) * 0x100)
#define DP_AUX_CH_DATA(port, i) (0x64014 + (port) * 0x100 + (i) * 4)
#define DP_AUX_CH_CTL_SEND_BUSY (1u << 31)
#define DP_AUX_CH_CTL_DONE (1 << 30)
#define DP_AUX_CH_CTL_INTERRUPT (1 << 29)
#define DP_AUX_CH_CTL_TIME_OUT_ERROR (1 << 28)
#define DP_AUX_CH_CTL_TIME_OUT_400us (0 << 26)
#define DP_AUX_CH_CTL_TIME_OUT_600us (1 << 26)
#define DP_AUX_CH_CTL_TIME_OUT_800us (2 << 26)
#define DP_AUX_CH_CTL_TIME_OUT_MAX (3 << 26)
#define DP_AUX_CH_CTL_TIME_OUT_MASK (3 << 26)
#define DP_AUX_CH_CTL_RECEIVE_ERROR (1 << 25)
#define DP_AUX_CH_CTL_MESSAGE_SIZE_MASK (0x1f << 20)
#define DP_AUX_CH_CTL_MESSAGE_SIZE_SHIFT 20
#define DP_AUX_CH_CTL_PRECHARGE_2US_MASK (0xf << 16)
#define DP_AUX_CH_CTL_PRECHARGE_2US_SHIFT 16
#define DP_AUX_CH_CTL_AUX_AKSV_SELECT (1 << 15)
#define DP_AUX_CH_CTL_MANCHESTER_TEST (1 << 14)
#define DP_AUX_CH_CTL_SYNC_TEST (1 << 13)
#define DP_AUX_CH_CTL_DEGLITCH_TEST (1 << 12)
#define DP_AUX_CH_CTL_PRECHARGE_TEST (1 << 11)
#define DP_AUX_CH_CTL_BIT_CLOCK_2X_MASK (0x7ff)
#define DP_AUX_CH_CTL_PSR_DATA_AUX_REG_SKL (1 << 14)
#define DP_AUX_CH_CTL_FS_DATA_AUX_REG_SKL (1 << 13)
#define DP_AUX_CH_CTL_GTC_DATA_AUX_REG_SKL (1 << 12)
#define DP_AUX_CH_CTL_TBT_IO (1 << 11)
#define DP_AUX_CH_CTL_FW_SYNC_PULSE_SKL_MASK (0x1f << 5)
#define DP_AUX_CH_CTL_FW_SYNC_PULSE_SKL(c) (((c) - 1) << 5)
#define DP_AUX_CH_CTL_SYNC_PULSE_SKL(c) ((c) - 1)

/* ---- transcoders and pipes -------------------------------------------------- */
#define TRANS_BASE(trans) ((trans) == TRANSCODER_EDP ? 0x6F000 : 0x60000 + (trans) * 0x1000)
#define TRANS_HTOTAL(trans) (TRANS_BASE(trans) + 0x000)
#define TRANS_HBLANK(trans) (TRANS_BASE(trans) + 0x004)
#define TRANS_HSYNC(trans) (TRANS_BASE(trans) + 0x008)
#define TRANS_VTOTAL(trans) (TRANS_BASE(trans) + 0x00C)
#define TRANS_VBLANK(trans) (TRANS_BASE(trans) + 0x010)
#define TRANS_VSYNC(trans) (TRANS_BASE(trans) + 0x014)
#define TRANS_VSYNCSHIFT(trans) (TRANS_BASE(trans) + 0x028)
#define TRANS_MULT(trans) (TRANS_BASE(trans) + 0x02C)
#define TRANS_DDI_FUNC_CTL(trans) ((trans) == TRANSCODER_EDP ? 0x6F400 : 0x60400 + (trans) * 0x1000)
#define TRANS_DDI_FUNC_ENABLE (1u << 31)
#define TRANS_DDI_PORT_MASK (7 << 28)
#define TRANS_DDI_PORT_SHIFT 28
#define TRANS_DDI_SELECT_PORT(x) ((x) << 28)
#define TRANS_DDI_PORT_NONE (0 << 28)
#define TRANS_DDI_MODE_SELECT_MASK (7 << 24)
#define TRANS_DDI_MODE_SELECT_HDMI (0 << 24)
#define TRANS_DDI_MODE_SELECT_DVI (1 << 24)
#define TRANS_DDI_MODE_SELECT_DP_SST (2 << 24)
#define TRANS_DDI_MODE_SELECT_DP_MST (3 << 24)
#define TRANS_DDI_MODE_SELECT_FDI (4 << 24)
#define TRANS_DDI_BPC_MASK (7 << 20)
#define TRANS_DDI_BPC_8 (0 << 20)
#define TRANS_DDI_BPC_10 (1 << 20)
#define TRANS_DDI_BPC_6 (2 << 20)
#define TRANS_DDI_BPC_12 (3 << 20)
#define TRANS_DDI_PVSYNC (1 << 17)
#define TRANS_DDI_PHSYNC (1 << 16)
#define TRANS_DDI_EDP_INPUT_MASK (7 << 12)
#define TRANS_DDI_EDP_INPUT_A_ON (0 << 12)
#define TRANS_DDI_EDP_INPUT_A_ONOFF (4 << 12)
#define TRANS_DDI_EDP_INPUT_B_ONOFF (5 << 12)
#define TRANS_DDI_EDP_INPUT_C_ONOFF (6 << 12)
#define TRANS_DDI_HDCP_SIGNALLING (1 << 9)
#define TRANS_DDI_DP_VC_PAYLOAD_ALLOC (1 << 8)
#define TRANS_DDI_HDMI_SCRAMBLER_CTS_ENABLE (1 << 7)
#define TRANS_DDI_HDMI_SCRAMBLER_RESET_FREQ (1 << 6)
#define TRANS_DDI_PORT_WIDTH_MASK (7 << 1)
#define TRANS_DDI_PORT_WIDTH(width) (((width) - 1) << 1)
#define TRANS_DDI_HDMI_SCRAMBLING (1 << 0)
#define TRANS_CONF(trans) ((trans) == TRANSCODER_EDP ? 0x7F008 : 0x70008 + (trans) * 0x1000)
#define TRANS_CONF_ENABLE (1u << 31)
#define TRANS_CONF_STATE_ENABLE (1 << 30)
#define TRANS_CONF_INTERLACE_MASK (7 << 21)
#define TRANS_CONF_PROGRESSIVE (0 << 21)
#define TRANS_CLK_SEL(trans) (0x46140 + (trans) * 4)
#define TRANS_CLK_SEL_DISABLED (0 << 29)
#define TRANS_CLK_SEL_PORT(x) (((x) + 1) << 29)
#define TRANS_MSA_MISC(trans) ((trans) == TRANSCODER_EDP ? 0x6F410 : 0x60410 + (trans) * 0x1000)
#define TRANS_MSA_SYNC_CLK (1 << 0)
#define TRANS_MSA_8_BPC (1 << 5)
#define TRANS_MSA_10_BPC (2 << 5)
#define TRANS_MSA_6_BPC (0 << 5)
#define PIPE_MISC(pipe) (0x70030 + (pipe) * 0x1000)
#define PIPE_MISC_BPC_MASK (7 << 5)
#define PIPE_MISC_BPC_8 (0 << 5)
#define PIPE_MISC_BPC_10 (1 << 5)
#define PIPE_MISC_BPC_6 (2 << 5)
#define PIPE_MISC_BPC_12 (3 << 5)
#define PIPE_MISC_DITHER_ENABLE (1 << 4)
#define PIPE_DSL(pipe) (0x70000 + (pipe) * 0x1000)
#define PIPE_FRMCOUNT(pipe) (0x70040 + (pipe) * 0x1000)
#define PIPE_FLIPCOUNT(pipe) (0x70044 + (pipe) * 0x1000)
#define PIPE_SCANLINE_MASK 0x1fff
#define TRANS_DATA_M1(trans) ((trans) == TRANSCODER_EDP ? 0x6F030 : 0x60030 + (trans) * 0x1000)
#define TRANS_DATA_N1(trans) ((trans) == TRANSCODER_EDP ? 0x6F034 : 0x60034 + (trans) * 0x1000)
#define TRANS_LINK_M1(trans) ((trans) == TRANSCODER_EDP ? 0x6F040 : 0x60040 + (trans) * 0x1000)
#define TRANS_LINK_N1(trans) ((trans) == TRANSCODER_EDP ? 0x6F044 : 0x60044 + (trans) * 0x1000)
#define TU_SIZE(x) (((x) - 1) << 25)
#define SKL_PS_CTRL(pipe, id) (0x68180 + (pipe) * 0x800 + (id) * 0x100)
#define PS_SCALER_EN (1u << 31)
#define PS_SCALER_MODE_HQ (1u << 30) /* scaler 0 on Gen9, or a pipe scaler */
#define PS_SCALER_MODE_DYN 0
#define PS_BINDING_PIPE 0 /* bits 27:25: 0 = the pipe, n+1 = plane n */
#define PS_FILTER_MEDIUM 0 /* bits 24:23 */
#define PS_FILTER_EDGE_ENHANCE (2u << 23)
#define PS_FILTER_BILINEAR (3u << 23)
#define SKL_PS_WIN_POS(pipe, id) (0x68170 + (pipe) * 0x800 + (id) * 0x100)
#define SKL_PS_WIN_SZ(pipe, id) (0x68174 + (pipe) * 0x800 + (id) * 0x100)
#define SKL_BOTTOM_COLOR(pipe) (0x70034 + (pipe) * 0x1000)

/* ---- planes (Skylake universal planes) ------------------------------------- */
#define PLANE_CTL_FORMAT_XRGB_8888 (4 << 24)
#define PLANE_CTL_FORMAT_XRGB_2101010 (2 << 24)
#define PLANE_CTL_FORMAT_XRGB_16161616F (6 << 24)
#define PLANE_CTL_FORMAT_RGB_565 (14 << 24)
#define PLANE_CTL_ORDER_RGBX (1 << 20)
#define PLANE_CTL_ALPHA_MASK (3 << 4)
#define PLANE_CTL_ALPHA_DISABLE (0 << 4)
#define PLANE_CTL_ALPHA_SW_PREMULTIPLY (2 << 4)
#define PLANE_CTL_ALPHA_HW_PREMULTIPLY (3 << 4)
#define PLANE_CTL_TILED_LINEAR (0 << 10)
#define PLANE_CTL_TILED_X (1 << 10)
#define PLANE_CTL_TILED_Y (4 << 10)
#define PLANE_CTL_TILED_YF (5 << 10)
#define PLANE_CTL_ROTATE_MASK 0x3
#define PLANE_CTL_ROTATE_0 0x0
#define PLANE_CTL_ROTATE_180 0x2
#define PLANE_CTL_PLANE_GAMMA_DISABLE (1 << 13)
#define PLANE_CTL_PIPE_GAMMA_ENABLE (1 << 30)
#define PLANE_CTL_PIPE_CSC_ENABLE (1 << 23)
/* The pipe's gamma table (Gen9: one 8-bit legacy palette per pipe, and a
 * mode register that says which of the tables is in use). */
#define LGC_PALETTE(pipe, i) (0x4a000 + (pipe) * 0x800 + (i) * 4)
#define GAMMA_MODE(pipe) (0x4a480 + (pipe) * 0x800)
#define GAMMA_MODE_MODE_8BIT 0
#define GAMMA_MODE_MODE_10BIT 1
#define GAMMA_MODE_MODE_12BIT 2
#define GAMMA_MODE_MODE_SPLIT 3
#define PLANE_CTL_KEY_ENABLE_MASK (3 << 21)
#define PLANE_KEYVAL(pipe, plane) (0x70194 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_KEYMSK(pipe, plane) (0x70198 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_KEYMAX(pipe, plane) (0x701a0 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_AUX_DIST(pipe, plane) (0x701c0 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_WM(pipe, plane, level) (0x70240 + (pipe) * 0x1000 + (plane) * 0x100 + (level) * 4)
#define PLANE_WM_TRANS(pipe, plane) (0x70268 + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_WM_EN (1u << 31)
#define PLANE_WM_LINES_SHIFT 14
#define PLANE_WM_LINES_MASK (0x1f << 14)
#define PLANE_WM_BLOCKS_MASK 0x7ff
#define PLANE_BUF_CFG(pipe, plane) (0x7027c + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_NV12_BUF_CFG(pipe, plane) (0x70278 + (pipe) * 0x1000 + (plane) * 0x100)
#define CUR_CTL(pipe) (0x70080 + (pipe) * 0x1000)
#define CUR_BASE(pipe) (0x70084 + (pipe) * 0x1000)
#define CUR_POS(pipe) (0x70088 + (pipe) * 0x1000)
#define CUR_FBC_CTL(pipe) (0x700a0 + (pipe) * 0x1000)
#define CUR_WM(pipe, level) (0x70140 + (pipe) * 0x1000 + (level) * 4)
#define CUR_WM_TRANS(pipe) (0x70168 + (pipe) * 0x1000)
#define CUR_BUF_CFG(pipe) (0x7017c + (pipe) * 0x1000)
#define CUR_MODE_MASK 0x27
#define CUR_MODE_DISABLE 0x00
#define CUR_MODE_64_ARGB_AX 0x27
#define CUR_MODE_128_ARGB_AX 0x22
#define CUR_MODE_256_ARGB_AX 0x23
#define CUR_GAMMA_ENABLE (1 << 26)
#define CUR_ROTATE_180 (1 << 15)
#define CUR_PIPE_SELECT(pipe) ((pipe) << 28)
#define CUR_POS_SIGN 0x8000
#define SKL_DDB_SIZE 896
#define PIPE_GMCH_DATA_M(pipe) 0

/* ---- the panel: power sequencing and backlight (PCH-side, SPT/CNP) ------ */
#define PP_STATUS 0xC7200
#define PP_ON (1u << 31)
#define PP_READY (1 << 30)
#define PP_SEQUENCE_MASK (3 << 28)
#define PP_SEQUENCE_NONE (0 << 28)
#define PP_SEQUENCE_POWER_UP (1 << 28)
#define PP_SEQUENCE_POWER_DOWN (2 << 28)
#define PP_CYCLE_DELAY_ACTIVE (1 << 27)
#define PP_SEQUENCE_STATE_MASK 0x0000000f
#define PP_SEQUENCE_STATE_ON_IDLE (0x8 << 0)
#define PP_CONTROL 0xC7204
#define PANEL_UNLOCK_REGS (0xabcd << 16)
#define PANEL_UNLOCK_MASK (0xffff << 16)
#define BXT_POWER_CYCLE_DELAY_MASK 0x1f0
#define BXT_POWER_CYCLE_DELAY_SHIFT 4
#define EDP_FORCE_VDD (1 << 3)
#define EDP_BLC_ENABLE (1 << 2)
#define PANEL_POWER_RESET (1 << 1)
#define PANEL_POWER_ON (1 << 0)
#define PP_ON_DELAYS 0xC7208
#define PANEL_PORT_SELECT_MASK (3 << 30)
#define PANEL_POWER_UP_DELAY_MASK 0x1fff0000
#define PANEL_POWER_UP_DELAY_SHIFT 16
#define PANEL_LIGHT_ON_DELAY_MASK 0x1fff
#define PANEL_LIGHT_ON_DELAY_SHIFT 0
#define PP_OFF_DELAYS 0xC720C
#define PANEL_POWER_DOWN_DELAY_MASK 0x1fff0000
#define PANEL_POWER_DOWN_DELAY_SHIFT 16
#define PANEL_LIGHT_OFF_DELAY_MASK 0x1fff
#define PANEL_LIGHT_OFF_DELAY_SHIFT 0
#define PP_DIVISOR 0xC7210
#define PP_REFERENCE_DIVIDER_MASK 0xffffff00
#define PP_REFERENCE_DIVIDER_SHIFT 8
#define PANEL_POWER_CYCLE_DELAY_MASK 0x1f
#define PANEL_POWER_CYCLE_DELAY_SHIFT 0
#define BLC_PWM_PCH_CTL1 0xC8250
#define BLM_PCH_PWM_ENABLE (1u << 31)
#define BLM_PCH_OVERRIDE_ENABLE (1 << 30)
#define BLM_PCH_POLARITY (1 << 29)
#define BLC_PWM_PCH_CTL2 0xC8254
#define BLC_PWM_CPU_CTL2 0x48250
#define BLM_PWM_ENABLE (1u << 31)
#define BLC_PWM_CPU_CTL 0x48254
#define SOUTH_CHICKEN1 0xC2000
#define SOUTH_CHICKEN2 0xC2004
#define LPT_PWM_GRANULARITY (1 << 5)
#define UTIL_PIN_CTL 0x48400
#define UTIL_PIN_ENABLE (1u << 31)
#define RAWCLK_FREQ 0xC6204
#define RAWCLK_FREQ_MASK 0x3ff

/* ---- hotplug (Sunrise Point family) ------------------------------------------ */
#define PCH_PORT_HOTPLUG 0xC4030
#define PORTA_HOTPLUG_ENABLE (1 << 28)
#define PORTB_HOTPLUG_ENABLE (1 << 12)
#define PORTC_HOTPLUG_ENABLE (1 << 4)
#define PORTD_HOTPLUG_ENABLE (1 << 20)
#define PORTB_HOTPLUG_STATUS_MASK (3 << 8)
#define PORTC_HOTPLUG_STATUS_MASK (3 << 0)
#define PORTD_HOTPLUG_STATUS_MASK (3 << 16)
#define PORTA_HOTPLUG_STATUS_MASK (3 << 24)
#define PCH_PORT_HOTPLUG2 0xC403C
#define PORTE_HOTPLUG_ENABLE (1 << 4)
#define PORTE_HOTPLUG_STATUS_MASK (3 << 0)
#define SDEISR_PORTA_HOTPLUG_LIVE (1 << 24)
#define SFUSE_STRAP 0xC2014
#define SFUSE_STRAP_DDIB_DETECTED (1 << 2)
#define SFUSE_STRAP_DDIC_DETECTED (1 << 1)
#define SFUSE_STRAP_DDID_DETECTED (1 << 0)
#define SDE_PORTA_HOTPLUG_LIVE (1 << 24)
#define SDE_PORTB_HOTPLUG_LIVE (1 << 21)
#define SDE_PORTC_HOTPLUG_LIVE (1 << 22)
#define SDE_PORTD_HOTPLUG_LIVE (1 << 23)
#define SDE_PORTE_HOTPLUG_LIVE (1 << 25)

/* ---- infoframes: the data-island packet transmitter of a transcoder ---------- */
#define HSW_TVIDEO_DIP_CTL(trans) (TRANS_BASE(trans) + 0x200)
#define HSW_TVIDEO_DIP_GCP(trans) (TRANS_BASE(trans) + 0x210)
#define HSW_TVIDEO_DIP_AVI_DATA(trans, i) (TRANS_BASE(trans) + 0x220 + (i) * 4)
#define HSW_TVIDEO_DIP_VS_DATA(trans, i) (TRANS_BASE(trans) + 0x260 + (i) * 4)
#define HSW_TVIDEO_DIP_SPD_DATA(trans, i) (TRANS_BASE(trans) + 0x2A0 + (i) * 4)
#define VIDEO_DIP_ENABLE_VSC_HSW (1 << 20)
#define VIDEO_DIP_ENABLE_GCP_HSW (1 << 16)
#define VIDEO_DIP_ENABLE_AVI_HSW (1 << 12)
#define VIDEO_DIP_ENABLE_VS_HSW (1 << 8)
#define VIDEO_DIP_ENABLE_GMP_HSW (1 << 4)
#define VIDEO_DIP_ENABLE_SPD_HSW (1 << 0)
#define VIDEO_DIP_DATA_SIZE 32

/* ---- the balance legs (current boost) of the DDI transmitters ---------------- */
#define DISPIO_CR_TX_BMU_CR0 0x6C00C
#define BALANCE_LEG_SHIFT(port) (8 + 3 * (port))
#define BALANCE_LEG_MASK(port) (7u << (8 + 3 * (port)))
#define BALANCE_LEG_DISABLE(port) (1u << (23 + (port)))
#define DDI_BUF_BALANCE_LEG_ENABLE (1u << 31)

/* hotplug pulse filters (PCH_PORT_HOTPLUG); 0 = 2 ms */
#define PORTB_PULSE_DURATION_MASK (3 << 10)
#define PORTC_PULSE_DURATION_MASK (3 << 2)
#define PORTD_PULSE_DURATION_MASK (3 << 18)
#define PORT_HOTPLUG_LONG_DETECT(shift) (2u << (shift))
#define PORT_HOTPLUG_SHORT_DETECT(shift) (1u << (shift))

/* ======================================================================
 * What the part needs set before it renders correctly, and what it
 * reports when it cannot reach memory
 * ====================================================================== */

/* A memory access the engine could not make: the address it wanted, in
 * which address space, and which engine asked. */
#define GEN8_RING_FAULT_REG 0x4094
#define RING_FAULT_VALID (1 << 0)
#define RING_FAULT_TYPE(x) (((x) >> 1) & 3)
#define RING_FAULT_SRCID(x) (((x) >> 3) & 0xff)
#define GEN8_RING_FAULT_ENGINE_ID(x) (((x) >> 12) & 7)
#define GEN8_FAULT_TLB_DATA0 0x4b10
#define GEN8_FAULT_TLB_DATA1 0x4b14
#define FAULT_VA_HIGH_BITS 0xf
#define FAULT_GTT_SEL (1 << 4)

/* Registers a batch from a client may write.  Everything privileged is
 * dropped unless it is named here; the graphics library writes a few of
 * them itself every time it sets up the pipeline. */
#define RING_FORCE_TO_NONPRIV(base, i) ((base) + 0x4d0 + (i) * 4)
#define RING_FORCE_TO_NONPRIV_COUNT 12

/* The chicken bits this generation needs.  Those whose value is carried
 * in the upper half as a write mask are set with I915_MASKED_ENABLE. */
#define MI_MODE_REG(base) ((base) + 0x9c)
#define ASYNC_FLIP_PERF_DISABLE (1 << 14)
#define GEN8_CS_CHICKEN1(base) ((base) + 0x580)
#define GEN9_PREEMPT_3D_OBJECT_LEVEL (1 << 0)
#define GEN9_PREEMPT_GPGPU_LEVEL(hi, lo) (((hi) << 2) | ((lo) << 1))
#define GEN9_PREEMPT_GPGPU_COMMAND_LEVEL GEN9_PREEMPT_GPGPU_LEVEL(1, 0)
#define GEN9_PREEMPT_GPGPU_LEVEL_MASK GEN9_PREEMPT_GPGPU_LEVEL(1, 1)
#define GEN9_CTX_PREEMPT_REG(base) ((base) + 0x248)
#define GEN9_CSFE_CHICKEN1_RCS(base) ((base) + 0xd4)
#define GEN9_PREEMPT_GPGPU_SYNC_SWITCH_DISABLE (1 << 2)
#define GEN8_ROW_CHICKEN 0xe4f0
#define PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE (1 << 8)
#define STALL_DOP_GATING_DISABLE (1 << 5)
#define HALF_SLICE_CHICKEN2 0xe180
#define GEN8_ST_PO_DISABLE (1 << 13)
#define HALF_SLICE_CHICKEN3 0xe184
#define GEN8_SAMPLER_POWER_BYPASS_DIS (1 << 1)
#define GEN9_DISABLE_OCL_OOB_SUPPRESS_LOGIC (1 << 5)
#define GEN9_HALF_SLICE_CHICKEN5 0xe188
#define GEN9_CCS_TLB_PREFETCH_ENABLE (1 << 3)
#define GEN9_HALF_SLICE_CHICKEN7 0xe194
#define GEN9_SAMPLER_HASH_COMPRESSED_READ_ADDR (1 << 8)
#define HDC_CHICKEN0 0x7300
#define HDC_FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT (1 << 5)
#define HDC_FORCE_CSR_NON_COHERENT_OVR_DISABLE (1 << 15)
#define GEN8_HDC_CHICKEN1 0x7304
#define CACHE_MODE_1 0x7004
#define GEN8_4x4_STC_OPTIMIZATION_DISABLE (1 << 6)
#define GEN9_PARTIAL_RESOLVE_IN_VC_DISABLE (1 << 1)
#define COMMON_SLICE_CHICKEN2 0x7014
#define GEN8_L3SQCREG4 0xb118
/* the memory object control tables (i915_mocs.c): one per engine at
 * base + instance * 0x100, and the L3 half for the render engine */
#define GEN9_RCS_MOCS_BASE 0xc800
#define GEN9_VCS_MOCS_BASE 0xc900
#define GEN9_VECS_MOCS_BASE 0xcb00
#define GEN9_BCS_MOCS_BASE 0xcc00
#define GEN9_LNCFCMOCS(i) (0xb020 + (i) * 4)
#define GEN8_LQSC_FLUSH_COHERENT_LINES (1u << 21)
#define BDW_SCRATCH1 0xb11c
#define GEN9_LBS_SLA_RETRY_TIMER_DECREMENT_ENABLE (1 << 2)
#define GAM_ECOCHK 0x4090
#define ECOCHK_DIS_TLB (1 << 8)
#define BDW_DISABLE_HDC_INVALIDATION (1u << 25)
#define GEN7_UCGCTL4 0x940c
#define GEN8_EU_GAUNIT_CLOCK_GATE_DISABLE (1 << 14)
#define MMCD_MISC_CTRL 0x4ddc
#define MMCD_PCLA (1u << 31)
#define MMCD_HOTSPOT_EN (1u << 27)
#define GEN9_GAMT_ECO_REG_RW_IA 0x4ab0
#define GAMT_ECO_ENABLE_IN_PLACE_DECOMPRESS (1 << 18)
#define GEN8_GARBCNTL 0xb004
/* the rest of the Gen9 settings: the context ones the batch that makes
 * the golden image loads, the ones loaded again at every context restore
 * (i915_lrc_wa_bb_gen9), and the engine ones written once */
#define FLOW_CONTROL_ENABLE (1 << 15) /* GEN8_ROW_CHICKEN */
#define HDC_FORCE_NON_COHERENT (1 << 4) /* HDC_CHICKEN0 */
#define GEN9_PBE_COMPRESSED_HASH_SELECTION (1 << 13) /* COMMON_SLICE_CHICKEN2 */
#define GEN9_DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE (1 << 12)
#define GEN9_ENABLE_YV12_BUGFIX (1 << 4) /* GEN9_HALF_SLICE_CHICKEN7 */
#define GEN9_ENABLE_GPGPU_PREEMPTION (1 << 2)
#define FF_SLICE_CHICKEN 0x2088
#define FF_SLICE_CHICKEN_CL_PROVOKING_VERTEX_FIX (1 << 1)
#define _3D_CHICKEN3 0x2090
#define _3D_CHICKEN_SF_PROVOKING_VERTEX_FIX (1 << 12)
#define GEN8_LQSQ_NONIA_COHERENT_ATOMICS_ENABLE (1u << 22) /* GEN8_L3SQCREG4 */
#define GEN8_L3SQCREG4_DEFAULT 0x40400000u /* what the part resets it to */
#define GEN9_SCRATCH_LNCF1 0xb008
#define GEN9_LNCF_NONIA_COHERENT_ATOMICS_ENABLE (1 << 0)
#define GEN9_SCRATCH1 0xb11c /* the same register as BDW_SCRATCH1 */
#define EVICTION_PERF_FIX_ENABLE (1 << 8)
/* Registers with one copy per slice or subslice: a read returns the copy
 * this selector names, a write goes to every copy. */
#define GEN8_MCR_SELECTOR 0xfdc
#define GEN8_MCR_SLICE(slice) (((slice) & 3u) << 26)
#define GEN8_MCR_SLICE_MASK GEN8_MCR_SLICE(3)
#define GEN8_MCR_SUBSLICE(subslice) (((subslice) & 3u) << 24)
#define GEN8_MCR_SUBSLICE_MASK GEN8_MCR_SUBSLICE(3)
/* Where in the register state the restore runs the indirect context
 * batch: an offset in 64-byte units, one per generation. */
#define GEN8_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT 0x17
#define GEN9_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT 0x26
#define GEN10_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT 0x19
#define GEN11_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT 0x1a
#define GEN12_CTX_RCS_INDIRECT_CTX_OFFSET_DEFAULT 0x0d
/* a scratch slot in a context's own status page (the first page of its
 * image) that the restore batch writes to */
#define LRC_PPHWSP_SCRATCH_ADDR (0x34 * 4)
/* render standby (RC6) and frequency control */
#define GEN6_RP_CONTROL 0xa024
/* Render P-states: the frequency the driver asks for, and what the part
 * can do.  Gen8 counts in 50 MHz steps at bit 25; Gen9 onwards in 50/3
 * MHz steps at bit 23. */
#define GEN6_RPNSWREQ 0xa008
#define GEN6_FREQUENCY(x) ((uint32_t)(x) << 25)
#define GEN9_FREQUENCY(x) ((uint32_t)(x) << 23)
#define GEN6_RP_STATE_CAP 0x145998 /* RP0 7:0, RP1 15:8, RPn 23:16 */
#define GEN6_RP_MEDIA_HW_NORMAL_MODE (2u << 9)
#define GEN6_RP_MEDIA_IS_GFX (1u << 8)
#define GEN6_RP_ENABLE (1u << 7)
#define GEN6_RP_DOWN_IDLE_AVG (0x2u << 0)
#define GEN6_RP_UP_BUSY_AVG (0x2u << 3)
#define GEN6_RP_INTERRUPT_LIMITS 0xa014
#define GEN6_RP_CUR_UP_EI 0xa050
#define GEN6_RPSTAT1 0xa01c
#define GEN9_CAGF_MASK (0x1ffu << 23)
#define GEN9_CAGF_SHIFT 23
#define GEN6_RC_CONTROL 0xa090
#define GEN6_RC_CTL_HW_ENABLE (1u << 31)
#define GEN6_RC_CTL_RC6_ENABLE (1u << 18)
#define GEN6_RC_STATE 0xa094
#define GEN6_GT_CORE_STATUS 0x138060
#define GEN7_L3CNTLREG 0x7034
#define GEN9_GAPS_TSV_CREDIT_DISABLE (1 << 7)

/* ---- GMBUS ------------------------------------------------------------------ */
#define PCH_GMBUS0 0xC5100
#define GMBUS_RATE_100KHZ (0 << 8)
#define GMBUS_RATE_50KHZ (1 << 8)
#define GMBUS_RATE_400KHZ (2 << 8)
#define GMBUS_HOLD_EXT (1 << 7)
#define GMBUS_BYTE_CNT_OVERRIDE (1 << 6)
#define GMBUS_PIN_DISABLED 0
#define GMBUS_PIN_SSC 1
#define GMBUS_PIN_VGADDC 2
#define GMBUS_PIN_PANEL 3
#define GMBUS_PIN_DPD_CHV 3
#define GMBUS_PIN_DPC 4
#define GMBUS_PIN_DPB 5
#define GMBUS_PIN_DPD 6
#define GMBUS_PIN_1_BXT 1
#define GMBUS_PIN_2_BXT 2
#define GMBUS_PIN_3_BXT 3
#define GMBUS_PIN_4_CNP 4
#define GMBUS_PIN_9_TC1_ICP 9
#define PCH_GMBUS1 0xC5104
#define GMBUS_SW_CLR_INT (1u << 31)
#define GMBUS_SW_RDY (1 << 30)
#define GMBUS_ENT (1 << 29)
#define GMBUS_CYCLE_WAIT (1 << 25)
#define GMBUS_CYCLE_INDEX (2 << 25)
#define GMBUS_CYCLE_STOP (4 << 25)
#define GMBUS_BYTE_COUNT_SHIFT 16
#define GMBUS_BYTE_COUNT_MAX 256U
#define GMBUS_SLAVE_INDEX_SHIFT 8
#define GMBUS_SLAVE_ADDR_SHIFT 1
#define GMBUS_SLAVE_READ (1 << 0)
#define GMBUS_SLAVE_WRITE (0 << 0)
#define PCH_GMBUS2 0xC5108
#define GMBUS_INUSE (1 << 15)
#define GMBUS_HW_WAIT_PHASE (1 << 14)
#define GMBUS_STALL_TIMEOUT (1 << 13)
#define GMBUS_INT (1 << 12)
#define GMBUS_HW_RDY (1 << 11)
#define GMBUS_SATOER (1 << 10)
#define GMBUS_ACTIVE (1 << 9)
#define PCH_GMBUS3 0xC510C
#define PCH_GMBUS4 0xC5110
#define GMBUS_NAK_EN (1 << 3)
#define GMBUS_IDLE_EN (1 << 2)
#define GMBUS_HW_WAIT_EN (1 << 1)
#define GMBUS_HW_RDY_EN (1 << 0)
#define PCH_GMBUS5 0xC5120
#define GMBUS_2BYTE_INDEX_EN (1u << 31)

/* ======================================================================
 * The GT: engines, rings, execution lists
 * ====================================================================== */

/* engine MMIO bases */
#define RENDER_RING_BASE 0x02000
#define BLT_RING_BASE 0x22000
/* the copy engine's two-dimensional commands (Gen8+ lengths, 64-bit addresses) */
#define XY_COLOR_BLT_CMD ((2u << 29) | (0x50u << 22) | (7 - 2))
#define XY_SRC_COPY_BLT_CMD ((2u << 29) | (0x53u << 22) | (10 - 2))
#define BLT_WRITE_ALPHA (1u << 21)
#define BLT_WRITE_RGB (1u << 20)
#define BLT_DEPTH_32 (3u << 24) /* in the second dword */
#define BLT_ROP_COLOR_COPY (0xf0u << 16)
#define BLT_ROP_SRC_COPY (0xccu << 16)
#define GEN6_BSD_RING_BASE 0x12000
#define GEN8_BSD2_RING_BASE 0x1c000
#define VEBOX_RING_BASE 0x1a000
#define GEN11_BSD_RING_BASE 0x1c0000
#define GEN11_BSD2_RING_BASE 0x1c4000
#define GEN11_BSD3_RING_BASE 0x1d0000
#define GEN11_BSD4_RING_BASE 0x1d4000
#define GEN11_VEBOX_RING_BASE 0x1c8000
#define GEN11_VEBOX2_RING_BASE 0x1d8000
#define GEN12_COMPUTE0_RING_BASE 0x1a000
#define GEN12_COMPUTE1_RING_BASE 0x1c000
#define GEN12_COMPUTE2_RING_BASE 0x1e000
#define GEN12_COMPUTE3_RING_BASE 0x26000

/* per-engine registers (offset from the base) */
#define RING_TAIL(base) ((base) + 0x30)
#define RING_HEAD(base) ((base) + 0x34)
#define RING_START(base) ((base) + 0x38)
#define RING_CTL(base) ((base) + 0x3c)
#define RING_CTL_SIZE(bytes) ((bytes) - 4096)
#define RING_VALID (1 << 0)
#define RING_PSMI_CTL(base) ((base) + 0x50)
#define RING_IPEIR(base) ((base) + 0x64) /* the instruction that faulted */
#define RING_IPEHR(base) ((base) + 0x68) /* the instruction being executed */
#define RING_INSTDONE(base) ((base) + 0x6c) /* which units are done (1 = idle) */
#define RING_INSTPS(base) ((base) + 0x70)
#define RING_BBADDR_UDW_(base) ((base) + 0x168)
#define GEN7_SC_INSTDONE 0x7100
#define GEN7_SAMPLER_INSTDONE 0xe160
#define GEN7_ROW_INSTDONE 0xe164
#define RING_ACTHD(base) ((base) + 0x74)
#define RING_ACTHD_UDW(base) ((base) + 0x5c)
#define RING_HWS_PGA(base) ((base) + 0x80)
#define RING_HWSTAM(base) ((base) + 0x98)
#define RING_MI_MODE(base) ((base) + 0x9c)
#define MODE_IDLE (1 << 9)
#define STOP_RING (1 << 8)
#define RING_IMR(base) ((base) + 0xa8)
#define RING_INSTPM(base) ((base) + 0xc0)
#define RING_RESET_CTL(base) ((base) + 0xd0)
#define RESET_CTL_CAT_ERROR (1 << 2)
#define RESET_CTL_READY_TO_RESET (1 << 1)
#define RESET_CTL_REQUEST_RESET (1 << 0)
#define RING_BBSTATE(base) ((base) + 0x110)
#define RING_BB_PPGTT (1 << 5)
#define RING_SBBADDR(base) ((base) + 0x114)
#define RING_SBBSTATE(base) ((base) + 0x118)
#define RING_SBBADDR_UDW(base) ((base) + 0x11c)
#define RING_BBADDR(base) ((base) + 0x140)
#define RING_BBADDR_UDW(base) ((base) + 0x168)
#define RING_BB_PER_CTX_PTR(base) ((base) + 0x1c0)
#define RING_INDIRECT_CTX(base) ((base) + 0x1c4)
#define RING_INDIRECT_CTX_OFFSET(base) ((base) + 0x1c8)
#define RING_ELSP(base) ((base) + 0x230)
#define RING_EXECLIST_STATUS_LO(base) ((base) + 0x234)
#define RING_EXECLIST_STATUS_HI(base) ((base) + 0x238)
#define RING_CONTEXT_CONTROL(base) ((base) + 0x244)
#define CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT (1 << 0)
#define CTX_CTRL_RS_CTX_ENABLE (1 << 1)
#define CTX_CTRL_INHIBIT_SYN_CTX_SWITCH (1 << 3)
#define CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT (1 << 2)
#define GEN12_CTX_CTRL_OAR_CONTEXT_ENABLE (1 << 8)
#define RING_PDP_UDW(base, n) ((base) + 0x270 + (n) * 8 + 4)
#define RING_PDP_LDW(base, n) ((base) + 0x270 + (n) * 8)
#define RING_MODE_GEN7(base) ((base) + 0x29c)
#define GFX_RUN_LIST_ENABLE (1 << 15)
#define GFX_INTERRUPT_STEERING (1 << 14)
#define GFX_TLB_INVALIDATE_EXPLICIT (1 << 13)
#define GFX_REPLAY_MODE (1 << 11)
#define GFX_PSMI_GRANULARITY (1 << 10)
#define GFX_PPGTT_ENABLE (1 << 9)
#define GEN8_GFX_PPGTT_48B (1 << 7)
#define GFX_FORWARD_VBLANK_MASK (3 << 5)
#define RING_CONTEXT_STATUS_BUF_LO(base, i) ((base) + 0x370 + (i) * 8)
#define RING_CONTEXT_STATUS_BUF_HI(base, i) ((base) + 0x374 + (i) * 8)
#define RING_CONTEXT_STATUS_PTR(base) ((base) + 0x3a0)
#define GEN8_CSB_ENTRIES 6
#define GEN11_CSB_ENTRIES 12
#define GEN8_CSB_WRITE_PTR_MASK (0xff)
#define GEN8_CSB_READ_PTR_MASK (0xff << 8)
#define RING_CTX_TIMESTAMP(base) ((base) + 0x3a8)
#define RING_EXECLIST_SQ_CONTENTS(base) ((base) + 0x510)
#define RING_EXECLIST_CONTROL(base) ((base) + 0x550)
#define EL_CTRL_LOAD (1 << 0)
#define RING_TIMESTAMP(base) ((base) + 0x358)
#define RING_TIMESTAMP_UDW(base) ((base) + 0x35c)
#define GEN8_RING_CS_GPR(base, n) ((base) + 0x600 + (n) * 8)
#define GEN9_RING_CS_GPR_UDW(base, n) ((base) + 0x604 + (n) * 8)
#define GEN12_RING_EIR(base) ((base) + 0xb0)
#define GEN12_RING_EMR(base) ((base) + 0xb4)
#define GEN12_RING_ESR(base) ((base) + 0xb8)
#define GEN8_R_PWR_CLK_STATE 0x20c8
#define GEN8_RPCS_ENABLE (1u << 31)
#define GEN8_RPCS_S_CNT_ENABLE (1u << 18)
#define GEN8_RPCS_S_CNT_SHIFT 15
#define GEN11_RPCS_S_CNT_SHIFT 12
#define GEN8_RPCS_SS_CNT_ENABLE (1u << 11)
#define GEN8_RPCS_SS_CNT_SHIFT 8
#define GEN8_RPCS_EU_MAX_SHIFT 4
#define GEN8_RPCS_EU_MIN_SHIFT 0

/* context status buffer entries */
#define GEN8_CTX_STATUS_IDLE_ACTIVE (1 << 0)
#define GEN8_CTX_STATUS_PREEMPTED (1 << 1)
#define GEN8_CTX_STATUS_ELEMENT_SWITCH (1 << 2)
#define GEN8_CTX_STATUS_ACTIVE_IDLE (1 << 3)
#define GEN8_CTX_STATUS_COMPLETE (1 << 4)
#define GEN8_CTX_STATUS_LITE_RESTORE (1 << 15)
#define GEN8_CTX_STATUS_COMPLETED_MASK (GEN8_CTX_STATUS_COMPLETE | GEN8_CTX_STATUS_PREEMPTED)

/* context descriptors */
#define GEN8_CTX_VALID (1 << 0)
#define GEN8_CTX_FORCE_PD_RESTORE (1 << 1)
#define GEN8_CTX_FORCE_RESTORE (1 << 2)
#define GEN8_CTX_ADDRESSING_MODE_SHIFT 3
#define GEN8_CTX_ADDRESSING_MODE_LEGACY32 (0 << 3)
#define GEN8_CTX_ADDRESSING_MODE_LEGACY64 (3 << 3)
#define GEN8_CTX_L3LLC_COHERENT (1 << 5)
#define GEN8_CTX_PRIVILEGE (1 << 8)
#define GEN8_CTX_ID_SHIFT 32
#define GEN8_CTX_ID_WIDTH 21
#define GEN11_SW_CTX_ID_SHIFT 37
#define GEN11_SW_CTX_ID_WIDTH 11
#define GEN11_ENGINE_INSTANCE_SHIFT 48
#define GEN11_ENGINE_CLASS_SHIFT 61

/* the hardware status page: dword indices */
#define I915_HWS_CSB_BUF0_INDEX 0x10
#define I915_HWS_CSB_WRITE_INDEX 0x1f
#define ICL_HWS_CSB_WRITE_INDEX 0x3f
#define I915_HWS_SEQNO_INDEX 0x40 /* byte 0x100 */
#define I915_HWS_SCRATCH_INDEX 0x80

/* command streamer instructions */
#define MI_INSTR(op, flags) (((op) << 23) | (flags))
#define MI_NOOP MI_INSTR(0, 0)
#define MI_USER_INTERRUPT MI_INSTR(0x02, 0)
#define MI_ARB_CHECK MI_INSTR(0x05, 0)
#define MI_ARB_ON_OFF MI_INSTR(0x08, 0)
#define MI_ARB_ENABLE (1 << 0)
#define MI_ARB_DISABLE (0 << 0)
#define MI_BATCH_BUFFER_END MI_INSTR(0x0a, 0)
#define MI_STORE_DWORD_IMM_GEN4 MI_INSTR(0x20, 2)
#define MI_STORE_DWORD_IMM_GEN8 MI_INSTR(0x20, 2)
#define MI_COPY_MEM_MEM MI_INSTR(0x2e, 3) /* one dword, memory to memory */
#define MI_USE_GGTT (1 << 22)
#define MI_LOAD_REGISTER_IMM(x) MI_INSTR(0x22, 2 * (x) - 1)
#define MI_LRI_FORCE_POSTED (1 << 12)
#define MI_STORE_REGISTER_MEM_GEN8 MI_INSTR(0x24, 2)
#define MI_LOAD_REGISTER_MEM_GEN8 MI_INSTR(0x29, 2)
#define MI_SRM_LRM_GLOBAL_GTT (1 << 22)
#define MI_FLUSH_DW MI_INSTR(0x26, 1)
#define MI_FLUSH_DW_STORE_INDEX (1 << 21)
#define MI_INVALIDATE_TLB (1 << 18)
#define MI_FLUSH_DW_OP_STOREDW (1 << 14)
#define MI_FLUSH_DW_USE_GTT (1 << 2)
#define MI_INVALIDATE_BSD (1 << 7)
#define MI_BATCH_BUFFER_START MI_INSTR(0x31, 0)
#define MI_BATCH_BUFFER_START_GEN8 MI_INSTR(0x31, 1)
#define MI_BATCH_PPGTT_HSW (1 << 8)
#define MI_BATCH_RESOURCE_STREAMER (1 << 10)
#define MI_BATCH_SECOND_LEVEL (1 << 22)

#define GFX_OP_PIPE_CONTROL(len) ((0x3 << 29) | (0x3 << 27) | (0x2 << 24) | ((len) - 2))
#define PIPE_CONTROL_COMMAND_CACHE_INVALIDATE (1 << 29)
#define PIPE_CONTROL_TILE_CACHE_FLUSH (1 << 28)
#define PIPE_CONTROL_FLUSH_L3 (1 << 27)
#define PIPE_CONTROL_GLOBAL_GTT_IVB (1 << 24)
#define PIPE_CONTROL_MMIO_WRITE (1 << 23)
#define PIPE_CONTROL_STORE_DATA_INDEX (1 << 21)
#define PIPE_CONTROL_CS_STALL (1 << 20)
#define PIPE_CONTROL_TLB_INVALIDATE (1 << 18)
#define PIPE_CONTROL_MEDIA_STATE_CLEAR (1 << 16)
#define PIPE_CONTROL_QW_WRITE (1 << 14)
#define PIPE_CONTROL_POST_SYNC_OP_MASK (3 << 14)
#define PIPE_CONTROL_DEPTH_STALL (1 << 13)
#define PIPE_CONTROL_WRITE_FLUSH (1 << 12)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH (1 << 12)
#define PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE (1 << 11)
#define PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE (1 << 10)
#define PIPE_CONTROL_INDIRECT_STATE_DISABLE (1 << 9)
#define PIPE_CONTROL_NOTIFY (1 << 8)
#define PIPE_CONTROL_FLUSH_ENABLE (1 << 7)
#define PIPE_CONTROL_DC_FLUSH_ENABLE (1 << 5)
#define PIPE_CONTROL_VF_CACHE_INVALIDATE (1 << 4)
#define PIPE_CONTROL_CONST_CACHE_INVALIDATE (1 << 3)
#define PIPE_CONTROL_STATE_CACHE_INVALIDATE (1 << 2)
#define PIPE_CONTROL_STALL_AT_SCOREBOARD (1 << 1)
#define PIPE_CONTROL_DEPTH_CACHE_FLUSH (1 << 0)
#define PIPE_CONTROL_GLOBAL_GTT (1 << 2)

/* PPGTT page table entries */
#define GEN8_PDE_PRESENT (1ULL << 0)
#define GEN8_PDE_RW (1ULL << 1)
#define GEN8_PTE_CACHE_PWT (1ULL << 3)
#define GEN8_PTE_CACHE_PCD (1ULL << 4)
#define GEN8_PTE_CACHE_PAT (1ULL << 7)
#define GEN8_PAGE_SIZE_2M (1ULL << 7)
#define GEN12_PPGTT_PTE_LM (1ULL << 11)

/* GT interrupt enables at the top level, Gen8-10 */
#define GEN8_GT_IRQ_BITS(shift) ((GT_RENDER_USER_INTERRUPT | GT_CONTEXT_SWITCH_INTERRUPT) << (shift))

/* ---- the other generations' display registers -------------------------------- */

/* Ports beyond E: the Type-C DDIs of Ice Lake (C-F) and Tiger Lake and
 * later (TC1-TC6 at indices 3-8). */
#define PORT_F 5
#define PORT_G 6
#define PORT_H 7
#define PORT_I 8
#define PORT_TC1 3

/* Power wells, Ice Lake and Tiger Lake: the pipe wells in the driver's
 * control register (indices below), the DDI and AUX wells in registers
 * of their own with the port as index. */
#define ICL_PW_CTL_IDX_PW_4 3
#define ICL_PW_CTL_IDX_PW_3 2
#define ICL_PW_CTL_IDX_PW_2 1
#define ICL_PW_CTL_IDX_PW_1 0
#define TGL_PW_CTL_IDX_PW_5 4
#define ICL_PWR_WELL_CTL_AUX1 0x45440
#define ICL_PWR_WELL_CTL_AUX2 0x45444 /* driver */
#define ICL_PWR_WELL_CTL_DDI1 0x45450
#define ICL_PWR_WELL_CTL_DDI2 0x45454 /* driver */
#define ICL_PW_CTL_IDX_DDI(port) (port) /* A=0 .. F=5; TC1.. at 3.. on TGL */
#define ICL_PW_CTL_IDX_AUX(port) (port)
#define TGL_PW_CTL_IDX_AUX_TBT(tc) (9 + (tc))
#define ICL_PW_CTL_IDX_AUX_TBT(tc) (8 + (tc))
/* Haswell/Broadwell: one well for everything but pipe A and DDI A. */
#define HSW_PW_CTL_IDX_GLOBAL 15
/* Broxton/Gemini Lake: PW1/PW2 at the Skylake indices; the two DPIO
 * PHYs are powered through the GT display power-on register. */
#define BXT_P_CR_GT_DISP_PWRON 0x138090
#define GT_DISPLAY_POWER_ON(phy) (1u << (phy))
#define BXT_PHY_CTL_FAMILY(phy) ((phy) == 0 ? 0x64C10 : 0x64C00)
#define COMMON_RESET_DIS (1u << 31)
#define BXT_PHY_BASE(phy) ((phy) == 0 ? 0x6C000 : 0x162000)
#define BXT_PORT_CL1CM_DW0(phy) (BXT_PHY_BASE(phy) + 0x0)
#define PHY_POWER_GOOD (1u << 6)
#define PHY_RESERVED (1u << 7)
#define BXT_PHY_CTL(port) (0x64C00 + (port) * 4)
#define BXT_PHY_CMNLANE_POWERDOWN_ACK (1u << 10)
#define BXT_PHY_LANE_POWERDOWN_ACK (1u << 9)
#define BXT_PHY_LANE_ENABLED (1u << 8)

/* Hotplug, Ice Point and later PCHs: one register for the DDI pins, one
 * for the Type-C pins, four bits per pin (status low two, enable bit 3). */
#define SHOTPLUG_CTL_DDI 0xC4030
#define SHOTPLUG_CTL_TC 0xC4034
#define ICP_HPD_ENABLE(pin) (0x8u << ((pin) * 4))
#define ICP_HPD_STATUS_MASK(pin) (0x3u << ((pin) * 4))
#define SDE_DDI_HOTPLUG_ICP(pin) (1u << (pin))
#define SDE_TC_HOTPLUG_ICP(tc) (1u << ((tc) + 16))
/* ...and the north display's Type-C pins (Gen11+). */
#define GEN11_TBT_HOTPLUG_CTL 0x44030
#define GEN11_TC_HOTPLUG_CTL 0x44038
#define GEN11_HOTPLUG_CTL_ENABLE(tc) (0x8u << ((tc) * 4))
#define GEN11_HOTPLUG_CTL_STATUS_MASK(tc) (0x3u << ((tc) * 4))
#define GEN11_TC_HOTPLUG(tc) (1u << ((tc) + 16))
#define GEN11_TBT_HOTPLUG(tc) (1u << (tc))
/* Broxton: the pins are in the north display, with the enables in the
 * register at the PCH hotplug address. */
#define BXT_HOTPLUG_CTL 0xC4030
#define BXT_DDIA_HPD_ENABLE (1u << 27)
#define BXT_DDIA_HPD_STATUS_MASK (3u << 24)
#define BXT_DDIC_HPD_ENABLE (1u << 11)
#define BXT_DDIC_HPD_STATUS_MASK (3u << 8)
#define BXT_DDIB_HPD_ENABLE (1u << 3)
#define BXT_DDIB_HPD_STATUS_MASK (3u << 0)
#define BXT_DE_PORT_HP_DDIA (1u << 3)
#define BXT_DE_PORT_HP_DDIB (1u << 4)
#define BXT_DE_PORT_HP_DDIC (1u << 5)

/* The backlight PWM of Broxton and of the Cannon Point family of PCHs:
 * a control, a frequency and a duty register per controller. */
#define BXT_BLC_PWM_CTL(c) (0xC8250 + (c) * 0x100)
#define BXT_BLC_PWM_FREQ(c) (0xC8254 + (c) * 0x100)
#define BXT_BLC_PWM_DUTY(c) (0xC8258 + (c) * 0x100)
#define BXT_BLC_PWM_ENABLE (1u << 31)
#define BXT_BLC_PWM_POLARITY (1u << 29)
/* Broxton's panel power sequencer lives in the north display. */
#define BXT_PP_BASE 0x61200
#define PCH_PP_BASE 0xC7200
#define PCH_PP_BASE2 0xC7300 /* Tiger Point: a second panel */

/* AUX channels of the Type-C ports on Tiger Lake and later. */
#define TGL_DP_AUX_CH_CTL(tc) (0x16F110 + (tc) * 0x100)
#define TGL_DP_AUX_CH_DATA(tc, i) (0x16F114 + (tc) * 0x100 + (i) * 4)

/* CDCLK sources: Haswell/Broadwell's LCPLL, Broxton's DE PLL, the CDCLK
 * PLL of Gen11+ with its reference from DSSM. */
#define LCPLL_CTL 0x130040
#define LCPLL_PLL_DISABLE (1u << 31)
#define LCPLL_CLK_FREQ_MASK (3u << 26)
#define LCPLL_CLK_FREQ_450 (0u << 26)
#define LCPLL_CLK_FREQ_54O_BDW (1u << 26)
#define LCPLL_CLK_FREQ_337_5_BDW (2u << 26)
#define LCPLL_CLK_FREQ_675_BDW (3u << 26)
#define LCPLL_CD_SOURCE_FCLK (1u << 21)
#define BXT_DE_PLL_ENABLE 0x46070 /* Gen11+: CDCLK_PLL_ENABLE */
#define BXT_DE_PLL_PLL_ENABLE (1u << 31)
#define BXT_DE_PLL_LOCK (1u << 30)
#define CDCLK_PLL_RATIO_MASK 0xff
#define BXT_DE_PLL_CTL 0x6D000
#define BXT_DE_PLL_RATIO_MASK 0xff
#define BXT_CDCLK_CD2X_DIV_SEL_MASK (3u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_1 (0u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_1_5 (1u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_2 (2u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_4 (3u << 22)
#define BXT_CDCLK_CD2X_PIPE_NONE (3u << 20)
#define BXT_CDCLK_SSA_PRECHARGE_ENABLE (1u << 16)
#define CDCLK_FREQ_DECIMAL_MASK 0x7ff
#define DSSM 0x51004
#define ICL_DSSM_CDCLK_PLL_REFCLK_MASK (7u << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_24MHz (0u << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz (1u << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz (2u << 29)

/* Tiger Lake and later: the transcoder names its port in four bits, the
 * DisplayPort transport control moves from the port to the transcoder,
 * and there is no embedded-panel transcoder (port A uses transcoder A). */
#define TGL_TRANS_DDI_PORT_MASK (0xfu << 27)
#define TGL_TRANS_DDI_PORT_SHIFT 27
#define TGL_TRANS_DDI_SELECT_PORT(port) (((port) + 1u) << 27)
#define TGL_TRANS_CLK_SEL_PORT(port) (((port) + 1u) << 28)
#define TGL_TRANS_CLK_SEL_DISABLED 0
#define TGL_DP_TP_CTL(trans) (0x60540 + (trans) * 0x1000)
#define TGL_DP_TP_STATUS(trans) (0x60544 + (trans) * 0x1000)
/* Gen11+: the plane's blending and gamma bits live in a register of
 * their own. */
#define PLANE_COLOR_CTL(pipe, plane) (0x701CC + (pipe) * 0x1000 + (plane) * 0x100)
#define PLANE_COLOR_PIPE_GAMMA_ENABLE (1u << 30)
#define PLANE_COLOR_PIPE_CSC_ENABLE (1u << 23)
#define PLANE_COLOR_PLANE_GAMMA_DISABLE (1u << 13)
#define PLANE_COLOR_ALPHA_DISABLE (0u << 4)
/* The display data buffer slices of Gen11+. */
#define DBUF_CTL_S1 0x45008
#define DBUF_CTL_S2 0x44FE8
#define DBUF_POWER_REQUEST (1u << 31)
#define DBUF_POWER_STATE (1u << 30)
#define ICL_DDB_SIZE 2048
#define TGL_DDB_SLICE_SIZE 1024
#define MBUS_CTL 0x4438C
#define MBUS_JOIN (1u << 31)

/* Haswell/Broadwell port clocks: a port takes the LCPLL directly for
 * DisplayPort, a WRPLL or the SPLL for the rest. */
#define PORT_CLK_SEL(port) (0x46100 + (port) * 4)
#define PORT_CLK_SEL_LCPLL_2700 (0u << 29)
#define PORT_CLK_SEL_LCPLL_1350 (1u << 29)
#define PORT_CLK_SEL_LCPLL_810 (2u << 29)
#define PORT_CLK_SEL_SPLL (3u << 29)
#define PORT_CLK_SEL_WRPLL(pll) ((4u + (pll)) << 29)
#define PORT_CLK_SEL_NONE (7u << 29)
#define PORT_CLK_SEL_MASK (7u << 29)
#define HSW_WRPLL_CTL(pll) ((pll) == 0 ? 0x46040 : 0x46060)
#define WRPLL_REF_LCPLL (3u << 28)
#define WRPLL_DIVIDER_REFERENCE(x) ((x) << 0)
#define WRPLL_DIVIDER_POST(x) ((x) << 8)
#define WRPLL_DIVIDER_FEEDBACK(x) ((x) << 16)
#define SPLL_CTL 0x46020
#define SPLL_PLL_ENABLE (1u << 31)
#define SPLL_REF_LCPLL (2u << 28)
#define SPLL_FREQ_810MHz (0u << 26)
#define SPLL_FREQ_1350MHz (1u << 26)
#define SPLL_FREQ_2700MHz (2u << 26)
/* Broadwell's primary plane (the older register set) and its watermarks. */
#define DSPCNTR(pipe) (0x70180 + (pipe) * 0x1000)
#define DSPLINOFF(pipe) (0x70184 + (pipe) * 0x1000)
#define DSPSTRIDE(pipe) (0x70188 + (pipe) * 0x1000)
#define DSPSURF(pipe) (0x7019C + (pipe) * 0x1000)
#define DSPTILEOFF(pipe) (0x701A4 + (pipe) * 0x1000)
#define DISP_ENABLE (1u << 31)
#define DISP_PIPE_GAMMA_ENABLE (1u << 30)
#define DISP_FORMAT_MASK (0xfu << 26)
#define DISP_FORMAT_BGRX565 (0x5u << 26)
#define DISP_FORMAT_BGRX888 (0x6u << 26)
#define DISP_FORMAT_RGBX888 (0xeu << 26)
#define DISP_FORMAT_BGRX101010 (0xau << 26)
#define DISP_FORMAT_RGBX101010 (0x8u << 26)
#define DISP_ROTATE_180 (1u << 15)
#define DISP_TILED (1u << 10)
#define WM0_PIPE_ILK(pipe) ((pipe) == 2 ? 0x45200 : 0x45100 + (pipe) * 4)
#define WM1_LP_ILK 0x45108
#define WM2_LP_ILK 0x4510C
#define WM3_LP_ILK 0x45110
#define WM_LP_ENABLE (1u << 31)
#define PIPE_WM_LINETIME(pipe) (0x45270 + (pipe) * 4)

/* ---- Ice Lake / Tiger Lake combo PHY PLLs and PHYs ---------------------------- */
#define ICL_DPLL_ENABLE(pll) (0x46010 + (pll) * 4) /* DPLL0, DPLL1 */
#define PLL_ENABLE (1u << 31)
#define PLL_LOCK (1u << 30)
#define PLL_POWER_ENABLE (1u << 27)
#define PLL_POWER_STATE (1u << 26)
#define TBT_PLL_ENABLE 0x46020
#define MG_PLL_ENABLE(tc) (0x46030 + (tc) * 4)
#define ICL_DPLL_CFGCR0(pll) (0x164000 + (pll) * 0x80)
#define ICL_DPLL_CFGCR1(pll) (0x164004 + (pll) * 0x80)
#define TGL_DPLL_CFGCR0(pll) (0x164284 + (pll) * 8)
#define TGL_DPLL_CFGCR1(pll) (0x164288 + (pll) * 8)
#define DPLL_CFGCR0_HDMI_MODE (1u << 30)
#define DPLL_CFGCR0_SSC_ENABLE_ICL (1u << 25)
#define DPLL_CFGCR0_LINK_RATE_MASK (0xfu << 25)
#define DPLL_CFGCR0_LINK_RATE_2700 (0u << 25)
#define DPLL_CFGCR0_LINK_RATE_1350 (1u << 25)
#define DPLL_CFGCR0_LINK_RATE_810 (2u << 25)
#define DPLL_CFGCR0_LINK_RATE_1620 (3u << 25)
#define DPLL_CFGCR0_LINK_RATE_1080 (4u << 25)
#define DPLL_CFGCR0_LINK_RATE_2160 (5u << 25)
#define DPLL_CFGCR0_LINK_RATE_3240 (6u << 25)
#define DPLL_CFGCR0_LINK_RATE_4050 (7u << 25)
#define DPLL_CFGCR0_DCO_FRACTION_MASK (0x7fffu << 10)
#define DPLL_CFGCR0_DCO_FRACTION(x) ((uint32_t)(x) << 10)
#define DPLL_CFGCR0_DCO_INTEGER_MASK 0x3ffu
#define DPLL_CFGCR1_QDIV_RATIO_MASK (0xffu << 10)
#define DPLL_CFGCR1_QDIV_RATIO(x) ((uint32_t)(x) << 10)
#define DPLL_CFGCR1_QDIV_MODE(x) ((uint32_t)(x) << 9)
#define DPLL_CFGCR1_KDIV_MASK (7u << 6)
#define DPLL_CFGCR1_KDIV(x) ((uint32_t)(x) << 6)
#define DPLL_CFGCR1_PDIV_MASK (0xfu << 2)
#define DPLL_CFGCR1_PDIV(x) ((uint32_t)(x) << 2)
#define DPLL_CFGCR1_CENTRAL_FREQ_8400 (3u << 0)
#define TGL_DPLL_CFGCR1_CFSELOVRD_NORMAL_XTAL (0u << 0)
#define ICL_DPCLKA_CFGCR0 0x164280
#define ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy) (1u << ((phy) + 10))
#define ICL_DPCLKA_CFGCR0_TC_CLK_OFF(tc) (1u << ((tc) + 12))
#define ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_MASK(phy) (3u << ((phy) * 2))
#define ICL_DPCLKA_CFGCR0_DDI_CLK_SEL(pll, phy) ((uint32_t)(pll) << ((phy) * 2))
#define ICL_DDI_CLK_SEL(port) (0x4610C + (port) * 4)
#define ICL_DDI_CLK_SEL_NONE (0x0u << 28)
#define ICL_DDI_CLK_SEL_MG (0x8u << 28)
#define ICL_DDI_CLK_SEL_TBT_162 (0xCu << 28)
#define ICL_DDI_CLK_SEL_TBT_270 (0xDu << 28)
#define ICL_DDI_CLK_SEL_TBT_540 (0xEu << 28)
#define ICL_DDI_CLK_SEL_TBT_810 (0xFu << 28)
#define ICL_DDI_CLK_SEL_MASK (0xFu << 28)
/* the combo PHY register file, one per combo port */
#define ICL_COMBOPHY_BASE(phy) ((phy) == 0 ? 0x162000 : (phy) == 1 ? 0x6C000 : \
				  (phy) == 2 ? 0x160000 : (phy) == 3 ? 0x161000 : 0x16B000)
#define ICL_PORT_CL_DW(dw, phy) (ICL_COMBOPHY_BASE(phy) + 4 * (dw))
#define ICL_PORT_COMP_DW(dw, phy) (ICL_COMBOPHY_BASE(phy) + 0x100 + 4 * (dw))
#define ICL_PORT_PCS_DW_AUX(dw, phy) (ICL_COMBOPHY_BASE(phy) + 0x300 + 4 * (dw))
#define ICL_PORT_PCS_DW_GRP(dw, phy) (ICL_COMBOPHY_BASE(phy) + 0x600 + 4 * (dw))
#define ICL_PORT_PCS_DW_LN(dw, ln, phy) (ICL_COMBOPHY_BASE(phy) + 0x800 + (ln) * 0x100 + 4 * (dw))
#define ICL_PORT_TX_DW_AUX(dw, phy) (ICL_COMBOPHY_BASE(phy) + 0x380 + 4 * (dw))
#define ICL_PORT_TX_DW_GRP(dw, phy) (ICL_COMBOPHY_BASE(phy) + 0x680 + 4 * (dw))
#define ICL_PORT_TX_DW_LN(dw, ln, phy) (ICL_COMBOPHY_BASE(phy) + 0x880 + (ln) * 0x100 + 4 * (dw))
#define ICL_PHY_MISC(port) (0x64C00 + (port) * 4)
#define ICL_PHY_MISC_DE_IO_COMP_PWR_DOWN (1u << 23)
#define COMP_INIT (1u << 31)
#define PROCESS_INFO_MASK (3u << 26)
#define PROCESS_INFO_SHIFT 26
#define VOLTAGE_INFO_MASK (3u << 24)
#define VOLTAGE_INFO_SHIFT 24
#define IREFGEN (1u << 24)
#define CL_POWER_DOWN_ENABLE (1u << 4)
#define SUS_CLOCK_CONFIG (3u << 0)
#define PWR_DOWN_LN_MASK (0xfu << 4)
#define PWR_DOWN_LN(x) ((uint32_t)(x) << 4)
#define COMMON_KEEPER_EN (1u << 26)
#define LATENCY_OPTIM (1u << 2)
#define SWING_SEL_UPPER(x) (((uint32_t)(x) >> 3) << 15)
#define SWING_SEL_UPPER_MASK (1u << 15)
#define SWING_SEL_LOWER(x) (((uint32_t)(x) & 0x7) << 11)
#define SWING_SEL_LOWER_MASK (0x7u << 11)
#define RCOMP_SCALAR(x) ((uint32_t)(x) << 0)
#define RCOMP_SCALAR_MASK 0xffu
#define LOADGEN_SELECT (1u << 31)
#define POST_CURSOR_1(x) ((uint32_t)(x) << 12)
#define POST_CURSOR_1_MASK (0x3fu << 12)
#define POST_CURSOR_2(x) ((uint32_t)(x) << 6)
#define POST_CURSOR_2_MASK (0x3fu << 6)
#define CURSOR_COEFF(x) ((uint32_t)(x) << 0)
#define CURSOR_COEFF_MASK 0x3fu
#define TX_TRAINING_EN (1u << 31)
#define TAP2_DISABLE (1u << 30)
#define TAP3_DISABLE (1u << 29)
#define SCALING_MODE_SEL(x) ((uint32_t)(x) << 18)
#define SCALING_MODE_SEL_MASK (0x7u << 18)
#define RTERM_SELECT(x) ((uint32_t)(x) << 3)
#define RTERM_SELECT_MASK (0x7u << 3)
#define N_SCALAR(x) ((uint32_t)(x) << 24)
#define N_SCALAR_MASK (0x7fu << 24)

/* ---- Broxton / Gemini Lake port PHY PLLs --------------------------------------- */
#define BXT_PORT_PLL_ENABLE(port) (0x46074 + (port) * 4)
#define PORT_PLL_ENABLE (1u << 31)
#define PORT_PLL_LOCK (1u << 30)
#define PORT_PLL_REF_SEL (1u << 27)
#define PORT_PLL_POWER_ENABLE (1u << 26)
#define PORT_PLL_POWER_STATE (1u << 25)
/* PHY0 (base 0x6C000) carries ports B (channel 0) and C (channel 1);
 * PHY1 (base 0x162000) carries port A on its channel 0. */
#define BXT_PORT_PHY(port) ((port) == PORT_A ? 1 : 0)
#define BXT_PORT_CH(port) ((port) == PORT_C ? 1 : 0)
#define BXT_PORT_PLL_EBB_0(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x34 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_PLL_EBB_4(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x38 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_PLL(port, n) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x100 + BXT_PORT_CH(port) * 0x300 + (n) * 4)
#define BXT_PORT_PCS_DW12_LN01(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x430 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_PCS_DW12_LN23(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x630 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_PCS_DW12_GRP(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x7030 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_PCS_DW10_LN01(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x428 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_PCS_DW10_GRP(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x7028 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_TX_DW2_LN0(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x508 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_TX_DW2_GRP(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x7108 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_TX_DW3_LN0(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x50C + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_TX_DW3_GRP(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x710C + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_TX_DW4_LN0(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x510 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_TX_DW4_GRP(port) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x7110 + BXT_PORT_CH(port) * 0x300)
#define BXT_PORT_TX_DW14_LN(port, ln) (BXT_PHY_BASE(BXT_PORT_PHY(port)) + 0x538 + (ln) * 0x80 + BXT_PORT_CH(port) * 0x300)
#define PORT_PLL_P1(x) ((uint32_t)(x) << 13)
#define PORT_PLL_P1_MASK (7u << 13)
#define PORT_PLL_P2(x) ((uint32_t)(x) << 8)
#define PORT_PLL_P2_MASK (0x1fu << 8)
#define PORT_PLL_10BIT_CLK_ENABLE (1u << 13)
#define PORT_PLL_RECALIBRATE (1u << 14)
#define PORT_PLL_M2_INT_MASK 0xffu
#define PORT_PLL_N(x) ((uint32_t)(x) << 8)
#define PORT_PLL_N_MASK (0xfu << 8)
#define PORT_PLL_M2_FRAC_MASK 0x3fffffu
#define PORT_PLL_M2_FRAC_ENABLE (1u << 16)
#define PORT_PLL_PROP_COEFF(x) ((uint32_t)(x) << 0)
#define PORT_PLL_PROP_COEFF_MASK 0xfu
#define PORT_PLL_INT_COEFF(x) ((uint32_t)(x) << 8)
#define PORT_PLL_INT_COEFF_MASK (0x1fu << 8)
#define PORT_PLL_GAIN_CTL(x) ((uint32_t)(x) << 16)
#define PORT_PLL_GAIN_CTL_MASK (7u << 16)
#define PORT_PLL_TARGET_CNT_MASK 0x3ffu
#define PORT_PLL_LOCK_THRESHOLD(x) ((uint32_t)(x) << 1)
#define PORT_PLL_LOCK_THRESHOLD_MASK (0xfu << 1)
#define PORT_PLL_DCO_AMP_OVR_EN_H (1u << 27)
#define PORT_PLL_DCO_AMP(x) ((uint32_t)(x) << 10)
#define PORT_PLL_DCO_AMP_MASK (0xfu << 10)
#define LANESTAGGER_STRAP_OVRD (1u << 6)
#define LANE_STAGGER_MASK 0x1fu
#define TX2_SWING_CALC_EN (1u << 31)
#define TX1_SWING_CALC_EN (1u << 30)
#define UNIQE_TRANGE_EN_METHOD (1u << 27)
#define MARGIN_000(x) ((uint32_t)(x) << 16)
#define UNIQ_TRANS_SCALE(x) ((uint32_t)(x) << 8)
#define DE_EMPHASIS(x) ((uint32_t)(x) << 24)
#define SCALE_DCOMP_METHOD (1u << 26)
#define LATENCY_OPTIM_BXT (1u << 25)
#define TX_MARGIN_MASK (0xffu << 16)

/* ---- Type-C: the flexible I/O adapter and the DKL/MG PHYs ---------------------- */
#define FIA_BASE(fia) ((fia) == 0 ? 0x163000 : (fia) == 1 ? 0x16E000 : 0x16F000)
#define PORT_TX_DFLEXDPMLE1(fia) (FIA_BASE(fia) + 0x8C0)
#define DFLEXDPMLE1_DPMLETC_MASK(tc) (0xfu << ((tc) * 4))
#define DFLEXDPMLE1_DPMLETC(tc, lanes) ((uint32_t)(lanes) << ((tc) * 4))
#define PORT_TX_DFLEXDPPMS(fia) (FIA_BASE(fia) + 0x890)
#define DP_PHY_MODE_STATUS_COMPLETED(tc) (1u << (tc))
#define PORT_TX_DFLEXDPCSSS(fia) (FIA_BASE(fia) + 0x894)
#define DP_PHY_MODE_STATUS_NOT_SAFE(tc) (1u << (tc))
#define PORT_TX_DFLEXDPSP(fia) (FIA_BASE(fia) + 0x8A0)
#define TC_LIVE_STATE_TBT(tc) (1u << ((tc) * 8 + 6))
#define TC_LIVE_STATE_TC(tc) (1u << ((tc) * 8 + 5))
#define DP_LANE_ASSIGNMENT_SHIFT(tc) ((tc) * 8)
#define DP_LANE_ASSIGNMENT_MASK(tc) (0xfu << ((tc) * 8))
#define PORT_TX_DFLEXPA1(fia) (FIA_BASE(fia) + 0x880)
#define DP_PIN_ASSIGNMENT_MASK(tc) (0xfu << ((tc) * 4))
#define DP_PIN_ASSIGNMENT_SHIFT(tc) ((tc) * 4)
/* Tiger Lake's DKL PHY: one register file per Type-C port, banked */
#define HIP_INDEX_REG(tc) ((tc) < 4 ? 0x1010A0 : 0x1010A4)
#define HIP_INDEX_VAL(tc, val) ((uint32_t)(val) << (((tc) % 4) * 8))
#define DKL_BASE(tc) (0x168000 + (tc) * 0x1000)
#define DKL_PLL_DIV0(tc) (DKL_BASE(tc) + 0x200)
#define DKL_PLL_DIV0_INTEG_COEFF(x) ((uint32_t)(x) << 16)
#define DKL_PLL_DIV0_PROP_COEFF(x) ((uint32_t)(x) << 12)
#define DKL_PLL_DIV0_FBPREDIV_SHIFT 8
#define DKL_PLL_DIV0_FBPREDIV(x) ((uint32_t)(x) << 8)
#define DKL_PLL_DIV0_FBDIV_INT(x) ((uint32_t)(x) << 0)
#define DKL_PLL_DIV1(tc) (DKL_BASE(tc) + 0x204)
#define DKL_PLL_DIV1_IREF_TRIM(x) ((uint32_t)(x) << 16)
#define DKL_PLL_DIV1_TDC_TARGET_CNT(x) ((uint32_t)(x) << 0)
#define DKL_PLL_SSC(tc) (DKL_BASE(tc) + 0x210)
#define DKL_PLL_SSC_IREF_NDIV_RATIO(x) ((uint32_t)(x) << 29)
#define DKL_PLL_SSC_STEP_LEN(x) ((uint32_t)(x) << 16)
#define DKL_PLL_SSC_STEP_NUM(x) ((uint32_t)(x) << 11)
#define DKL_PLL_SSC_EN (1u << 9)
#define DKL_PLL_BIAS(tc) (DKL_BASE(tc) + 0x214)
#define DKL_PLL_BIAS_FRAC_EN_H (1u << 30)
#define DKL_PLL_BIAS_FBDIV_FRAC(x) ((uint32_t)(x) << 8)
#define DKL_PLL_TDC_COLDST_BIAS(tc) (DKL_BASE(tc) + 0x218)
#define DKL_PLL_TDC_SSC_STEP_SIZE(x) ((uint32_t)(x) << 8)
#define DKL_PLL_TDC_FEED_FWD_GAIN(x) ((uint32_t)(x) << 0)
#define DKL_REFCLKIN_CTL(tc) (DKL_BASE(tc) + 0x12C)
#define DKL_CLKTOP2_HSCLKCTL(tc) (DKL_BASE(tc) + 0xD4)
#define DKL_CLKTOP2_CORECLKCTL1(tc) (DKL_BASE(tc) + 0xD8)
#define DKL_TX_PMD_LANE_SUS(tc) (DKL_BASE(tc) + 0xD00)
#define DKL_TX_DPCNTL0(tc) (DKL_BASE(tc) + 0x2C0)
#define DKL_TX_DPCNTL1(tc) (DKL_BASE(tc) + 0x2C4)
#define DKL_TX_DPCNTL2(tc) (DKL_BASE(tc) + 0x2C8)
#define DKL_TX_PRESHOOT_COEFF(x) ((uint32_t)(x) << 13)
#define DKL_TX_DE_EMPAHSIS_COEFF(x) ((uint32_t)(x) << 7)
#define DKL_TX_VSWING_CONTROL(x) ((uint32_t)(x) << 0)
#define DKL_TX_DP20BITMODE (1u << 2)
#define DKL_TX_LOADGEN_SHARING_PMD_DISABLE (1u << 12)
/* the shared clock-top words used by both MG and DKL */
#define MG_REFCLKIN_CTL_OD_2_MUX(x) ((uint32_t)(x) << 8)
#define MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO(x) ((uint32_t)(x) << 8)
#define MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL(x) ((uint32_t)(x) << 16)
#define MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL(x) ((uint32_t)(x) << 14)
#define MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_2 (0u << 12)
#define MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_3 (1u << 12)
#define MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_5 (2u << 12)
#define MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_7 (3u << 12)
#define MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO(x) ((uint32_t)(x) << 8)
/* Ice Lake's MG PHY register file per Type-C port */
#define MG_BASE(tc) (0x168000 + (tc) * 0x1000)
#define MG_REFCLKIN_CTL(tc) (MG_BASE(tc) + 0x92C)
#define MG_CLKTOP2_CORECLKCTL1(tc) (MG_BASE(tc) + 0x8D8)
#define MG_CLKTOP2_HSCLKCTL(tc) (MG_BASE(tc) + 0x8D4)
#define MG_PLL_DIV0(tc) (MG_BASE(tc) + 0xA00)
#define MG_PLL_DIV0_FRACNEN_H (1u << 30)
#define MG_PLL_DIV0_FBDIV_FRAC(x) ((uint32_t)(x) << 8)
#define MG_PLL_DIV0_FBDIV_INT(x) ((uint32_t)(x) << 0)
#define MG_PLL_DIV1(tc) (MG_BASE(tc) + 0xA04)
#define MG_PLL_DIV1_IREF_NDIVRATIO(x) ((uint32_t)(x) << 16)
#define MG_PLL_DIV1_DITHER_DIV_2 (1u << 12)
#define MG_PLL_DIV1_NDIVRATIO(x) ((uint32_t)(x) << 4)
#define MG_PLL_DIV1_FBPREDIV(x) ((uint32_t)(x) << 0)
#define MG_PLL_LF(tc) (MG_BASE(tc) + 0xA08)
#define MG_PLL_LF_TDCTARGETCNT(x) ((uint32_t)(x) << 24)
#define MG_PLL_LF_AFCCNTSEL_512 (1u << 20)
#define MG_PLL_LF_GAINCTRL(x) ((uint32_t)(x) << 16)
#define MG_PLL_LF_INT_COEFF(x) ((uint32_t)(x) << 8)
#define MG_PLL_LF_PROP_COEFF(x) ((uint32_t)(x) << 0)
#define MG_PLL_FRAC_LOCK(tc) (MG_BASE(tc) + 0xA0C)
#define MG_PLL_FRAC_LOCK_TRUELOCK_CRIT_32 (1u << 18)
#define MG_PLL_FRAC_LOCK_EARLYLOCK_CRIT_32 (1u << 16)
#define MG_PLL_FRAC_LOCK_LOCKTHRESH(x) ((uint32_t)(x) << 11)
#define MG_PLL_FRAC_LOCK_DCODITHEREN (1u << 10)
#define MG_PLL_FRAC_LOCK_FEEDFWRDCAL_EN (1u << 9)
#define MG_PLL_FRAC_LOCK_FEEDFWRDGAIN(x) ((uint32_t)(x) << 0)
#define MG_PLL_SSC(tc) (MG_BASE(tc) + 0xA10)
#define MG_PLL_BIAS(tc) (MG_BASE(tc) + 0xA14)
#define MG_PLL_BIAS_BIAS_GB_SEL(x) ((uint32_t)(x) << 30)
#define MG_PLL_BIAS_INIT_DCOAMP(x) ((uint32_t)(x) << 24)
#define MG_PLL_BIAS_BIAS_BONUS(x) ((uint32_t)(x) << 16)
#define MG_PLL_BIAS_BIASCAL_EN (1u << 15)
#define MG_PLL_BIAS_CTRIM(x) ((uint32_t)(x) << 8)
#define MG_PLL_BIAS_VREF_RDAC(x) ((uint32_t)(x) << 5)
#define MG_PLL_BIAS_IREFTRIM(x) ((uint32_t)(x) << 0)
#define MG_PLL_TDC_COLDST_BIAS(tc) (MG_BASE(tc) + 0xA18)
#define MG_PLL_TDC_COLDST_IREFINT_EN (1u << 27)
#define MG_PLL_TDC_COLDST_REFBIAS_START_PULSE_W(x) ((uint32_t)(x) << 17)
#define MG_PLL_TDC_COLDST_COLDSTART (1u << 16)
#define TGL_TBTPLL_CFGCR0 0x16429C
#define TGL_TBTPLL_CFGCR1 0x1642A0
#define ICL_TBTPLL_CFGCR0 0x164100 /* "DPLL 2" in the combo file */
#define ICL_TBTPLL_CFGCR1 0x164104
#define TBT_PLL_ENABLE 0x46020
#define MODULAR_FIA_MASK (1u << 4)

/* ---- Type-C PHY details (the MG and DKL lane/mode words) ---------------------- */
#define MG_LN(tc, ln) (MG_BASE(tc) + (ln) * 0x400)
#define MG_TX1_LINK_PARAMS(tc, ln) (MG_LN(tc, ln) + 0x12C)
#define MG_TX2_LINK_PARAMS(tc, ln) (MG_LN(tc, ln) + 0x0AC)
#define CRI_USE_FS32 (1u << 5)
#define MG_TX1_PISO_READLOAD(tc, ln) (MG_LN(tc, ln) + 0x14C)
#define MG_TX2_PISO_READLOAD(tc, ln) (MG_LN(tc, ln) + 0x0CC)
#define CRI_CALCINIT (1u << 1)
#define MG_TX1_SWINGCTRL(tc, ln) (MG_LN(tc, ln) + 0x148)
#define MG_TX2_SWINGCTRL(tc, ln) (MG_LN(tc, ln) + 0x0C8)
#define CRI_TXDEEMPH_OVERRIDE_17_12(x) ((uint32_t)(x) << 0)
#define CRI_TXDEEMPH_OVERRIDE_17_12_MASK (0x3Fu << 0)
#define MG_TX1_DRVCTRL(tc, ln) (MG_LN(tc, ln) + 0x144)
#define MG_TX2_DRVCTRL(tc, ln) (MG_LN(tc, ln) + 0x0C4)
#define CRI_TXDEEMPH_OVERRIDE_11_6(x) ((uint32_t)(x) << 24)
#define CRI_TXDEEMPH_OVERRIDE_11_6_MASK (0x3Fu << 24)
#define CRI_TXDEEMPH_OVERRIDE_EN (1u << 22)
#define CRI_TXDEEMPH_OVERRIDE_5_0(x) ((uint32_t)(x) << 16)
#define CRI_TXDEEMPH_OVERRIDE_5_0_MASK (0x3Fu << 16)
#define MG_CLKHUB(tc, ln) (MG_LN(tc, ln) + 0x79C)
#define CFG_LOW_RATE_LKREN_EN (1u << 11)
#define MG_TX1_DCC(tc, ln) (MG_LN(tc, ln) + 0x110)
#define MG_TX2_DCC(tc, ln) (MG_LN(tc, ln) + 0x090)
#define CFG_AMI_CK_DIV_OVERRIDE_VAL(x) ((uint32_t)(x) << 25)
#define CFG_AMI_CK_DIV_OVERRIDE_VAL_MASK (0x3u << 25)
#define CFG_AMI_CK_DIV_OVERRIDE_EN (1u << 24)
#define MG_DP_MODE(tc, ln) (MG_LN(tc, ln) + 0x3A0)
#define MG_DP_MODE_CFG_DP_X2_MODE (1u << 7)
#define MG_DP_MODE_CFG_DP_X1_MODE (1u << 6)
#define MG_DP_MODE_CFG_TR2PWR_GATING (1u << 5)
#define MG_DP_MODE_CFG_TRPWR_GATING (1u << 4)
#define MG_DP_MODE_CFG_CLNPWR_GATING (1u << 3)
#define MG_DP_MODE_CFG_DIGPWR_GATING (1u << 2)
#define MG_DP_MODE_CFG_GAONPWR_GATING (1u << 1)
#define MG_MISC_SUS0(tc) (MG_BASE(tc) + 0x814)
#define MG_MISC_SUS0_SUSCLK_DYNCLKGATE_MODE_MASK (3u << 14)
#define MG_MISC_SUS0_SUSCLK_DYNCLKGATE_MODE(x) ((uint32_t)(x) << 14)
#define MG_MISC_SUS0_CFG_TR2PWR_GATING (1u << 12)
#define MG_MISC_SUS0_CFG_CL2PWR_GATING (1u << 11)
#define MG_MISC_SUS0_CFG_GAONPWR_GATING (1u << 10)
#define MG_MISC_SUS0_CFG_TRPWR_GATING (1u << 7)
#define MG_MISC_SUS0_CFG_CL1PWR_GATING (1u << 6)
#define MG_MISC_SUS0_CFG_DGPWR_GATING (1u << 5)
#define MG_PLL_SSC_EN (1u << 28)
#define MG_PLL_SSC_TYPE(x) ((uint32_t)(x) << 26)
#define MG_PLL_SSC_STEPLENGTH(x) ((uint32_t)(x) << 16)
#define MG_PLL_SSC_STEPNUM(x) ((uint32_t)(x) << 10)
#define MG_PLL_SSC_FLLEN (1u << 9)
#define MG_PLL_SSC_STEPSIZE(x) ((uint32_t)(x) << 0)
#define MG_PLL_TDC_TDCOVCCORR_EN (1u << 2)
#define MG_PLL_TDC_TDCSEL(x) ((uint32_t)(x) << 0)
#define MG_REFCLKIN_CTL_OD_2_MUX_MASK (7u << 8)
#define MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO_MASK (0xffu << 8)
#define MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL_MASK (3u << 16)
#define MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL_MASK (1u << 14)
#define MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_MASK (3u << 12)
#define MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_MASK (0xfu << 8)
#define DKL_DP_MODE(tc) (DKL_BASE(tc) + 0xA0)
#define DKL_PLL_DIV0_MASK 0x1ffffffu
#define DKL_PLL_DIV1_IREF_TRIM_MASK (0x7fu << 16)
#define DKL_PLL_DIV1_TDC_TARGET_CNT_MASK 0xffu
#define DKL_PLL_SSC_IREF_NDIV_RATIO_MASK (7u << 29)
#define DKL_PLL_SSC_STEP_LEN_MASK (0xffu << 16)
#define DKL_PLL_SSC_STEP_NUM_MASK (7u << 11)
#define DKL_PLL_BIAS_FBDIV_FRAC_MASK (0x3fffffu << 8)
#define DKL_PLL_TDC_SSC_STEP_SIZE_MASK (0xffu << 8)
#define DKL_PLL_TDC_FEED_FWD_GAIN_MASK 0xffu
#define DKL_TX_PRESHOOT_COEFF_MASK (0x1fu << 13)
#define DKL_TX_DE_EMPAHSIS_COEFF_MASK (0x1fu << 7)
#define DKL_TX_VSWING_CONTROL_MASK (7u << 0)

/* ---- the GuC and HuC ------------------------------------------------------------ */
#define GUC_STATUS 0xc000
#define GS_MIA_IN_RESET (1u << 0)
#define GS_BOOTROM_MASK (0x7fu << 1)
#define GS_BOOTROM_RSA_FAILED (0x50u << 1)
#define GS_BOOTROM_JUMP_PASSED (0x60u << 1)
#define GS_UKERNEL_SHIFT 8
#define GS_UKERNEL_MASK (0xffu << 8)
#define GS_MIA_MASK (7u << 16)
#define GS_AUTH_STATUS_MASK (3u << 30)
#define GS_AUTH_STATUS_BAD (1u << 30)
#define GS_AUTH_STATUS_GOOD (2u << 30)
#define GUC_LOAD_STATUS_READY 0xf0
#define SOFT_SCRATCH(n) (0xc180 + (n) * 4)
#define GEN11_SOFT_SCRATCH(n) (0x190240 + (n) * 4)
#define UOS_RSA_SCRATCH(i) (0xc200 + (i) * 4)
#define UOS_RSA_SCRATCH_COUNT 64
#define DMA_ADDR_0_LOW 0xc300
#define DMA_ADDR_0_HIGH 0xc304
#define DMA_ADDR_1_LOW 0xc308
#define DMA_ADDR_1_HIGH 0xc30c
#define DMA_ADDRESS_SPACE_WOPCM (7u << 16)
#define DMA_ADDRESS_SPACE_GTT (8u << 16)
#define DMA_COPY_SIZE 0xc310
#define DMA_CTRL 0xc314
#define HUC_UKERNEL (1u << 9)
#define UOS_MOVE (1u << 4)
#define START_DMA (1u << 0)
#define DMA_GUC_WOPCM_OFFSET 0xc340
#define GUC_WOPCM_OFFSET_VALID (1u << 0)
#define HUC_LOADING_AGENT_GUC (1u << 1)
#define GUC_WOPCM_OFFSET_MASK (0x3ffffu << 14)
#define GUC_MAX_IDLE_COUNT 0xc3e4
#define GUC_WOPCM_SIZE 0xc050
#define GUC_WOPCM_SIZE_LOCKED (1u << 0)
#define GUC_WOPCM_SIZE_MASK (0xfffffu << 12)
#define GUC_SHIM_CONTROL 0xc064
#define GUC_DISABLE_SRAM_INIT_TO_ZEROES (1u << 0)
#define GUC_ENABLE_READ_CACHE_LOGIC (1u << 1)
#define GUC_ENABLE_MIA_CACHING (1u << 2)
#define GUC_GEN10_MSGCH_ENABLE (1u << 4)
#define GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA (1u << 9)
#define GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA (1u << 10)
#define GUC_ENABLE_MIA_CLOCK_GATING (1u << 15)
#define GUC_SEND_INTERRUPT 0xc4c8
#define GUC_SEND_TRIGGER (1u << 0)
#define GEN11_GUC_HOST_INTERRUPT 0x1901f0
#define GUC_INTR_GUC2HOST (1u << 15)
#define GUC_ARAT_C6DIS 0xa178
#define HUC_STATUS2 0xd3b0
#define HUC_FW_VERIFIED (1u << 7)
#define GEN11_HUC_KERNEL_LOAD_INFO 0xc1dc
#define HUC_LOAD_SUCCESSFUL (1u << 0)
#define GEN9_GT_PM_CONFIG 0x13816c
#define GEN9LP_GT_PM_CONFIG 0x138140
#define GT_DOORBELL_ENABLE (1u << 0)
#define GEN7_MISCCPCTL 0x9424
#define GEN8_DOP_CLOCK_GATE_GUC_ENABLE (1u << 4)
#define GEN8_GTCR 0x4274
#define GEN8_GTCR_INVALIDATE (1u << 0)
#define GEN12_GUC_TLB_INV_CR 0xcee8
#define GEN12_GUC_TLB_INV_CR_INVALIDATE (1u << 0)
#define GEN11_GRDOM_GUC (1u << 3)
