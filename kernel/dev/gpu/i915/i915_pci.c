// LikeOS -- the Intel graphics device table.
//
// Every integrated and discrete Intel graphics device from the i810 to
// Nova Lake, and the data-centre Ponte Vecchio and Crescent Island parts,
// by PCI device id, with the descriptor that says which generation it is
// and what the driver can do with it.  The driver runs generation 2
// (i830) through Xe3 (Lunar Lake, Battlemage, Panther Lake, Wildcat Lake,
// Nova Lake-S); Nova Lake-P's display is driven and its GT left off; the
// i810/i815 and the compute-only parts are named in the log and left on
// the boot framebuffer.
//
// Firmware names are relative to /lib/firmware: "i915/..." for the parts
// up to Meteor Lake and for every display microcode, "xe/..." for the GuC,
// HuC and GSC images of Lunar Lake and later.
//
// The table is ours: ids and names from the public product lists, in a
// layout that carries the SKU's GT level and a marketing name where one
// helps a log line be recognisable.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_device_info.h>

#define NAME_ONLY(platform_, name_, gen_)                                  \
	{                                                                  \
		.name = name_, .platform = platform_, .gen = gen_,         \
		.gen_x10 = (gen_) * 10, .flags = I915_INFO_NAME_ONLY,      \
	}

/* ---- generation 1: identified only -------------------------------------- */

static const struct intel_device_info info_i810 = NAME_ONLY(I915_PLATFORM_I810, "i810", 1);
static const struct intel_device_info info_i815 = NAME_ONLY(I915_PLATFORM_I815, "i815", 1);

/* ---- generations 2-7 ----------------------------------------------------
 *
 * Ring buffers instead of execution lists, the global GTT (and on Gen6/7
 * one PPGTT every context shares) instead of per-context spaces: the GT
 * of these is i915_legacy_*.c, the display intel_legacy_*.c (Haswell's
 * is the DDI display, with Broadwell's model). */

#define LEGACY_GEN(gen_, x10_, dv_)                                         \
	.gen = gen_, .gen_x10 = x10_, .display_ver = dv_,                    \
	.dpll_model = I915_DPLL_LEGACY

static const struct intel_device_info info_i830 = {
	.name = "i830",
	.platform = I915_PLATFORM_I830,
	LEGACY_GEN(2, 20, 2),
	.num_pipes = 2,
	.flags = I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0,
	.dma_mask_bits = 32,
};

static const struct intel_device_info info_i845g = {
	.name = "i845G",
	.platform = I915_PLATFORM_I845G,
	LEGACY_GEN(2, 20, 2),
	.num_pipes = 1,
	.flags = I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0,
	.dma_mask_bits = 32,
};

static const struct intel_device_info info_i85x = {
	.name = "i85x",
	.platform = I915_PLATFORM_I85X,
	LEGACY_GEN(2, 20, 2),
	.num_pipes = 2,
	.flags = I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0,
	.dma_mask_bits = 32,
};

static const struct intel_device_info info_i865g = {
	.name = "i865G",
	.platform = I915_PLATFORM_I865G,
	LEGACY_GEN(2, 20, 2),
	.num_pipes = 1,
	.flags = I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0,
	.dma_mask_bits = 32,
};

#define GEN3_INFO(platform_, name_, dma_)                                   \
	{                                                                    \
		.name = name_, .platform = platform_, LEGACY_GEN(3, 30, 3),  \
		.num_pipes = 2, .flags = I915_INFO_HAS_SNOOP,                \
		.engine_mask = I915_ENGINE_RCS0, .dma_mask_bits = dma_,      \
	}

static const struct intel_device_info info_i915g = GEN3_INFO(I915_PLATFORM_I915G, "i915G", 32);
static const struct intel_device_info info_i915gm = GEN3_INFO(I915_PLATFORM_I915GM, "i915GM", 32);
static const struct intel_device_info info_i945g = GEN3_INFO(I915_PLATFORM_I945G, "i945G", 32);
static const struct intel_device_info info_i945gm = GEN3_INFO(I915_PLATFORM_I945GM, "i945GM", 32);
static const struct intel_device_info info_g33 = GEN3_INFO(I915_PLATFORM_G33, "G33", 36);
static const struct intel_device_info info_pineview = GEN3_INFO(I915_PLATFORM_PINEVIEW, "Pineview", 36);

static const struct intel_device_info info_i965g = {
	.name = "i965G",
	.platform = I915_PLATFORM_I965G,
	LEGACY_GEN(4, 40, 4),
	.num_pipes = 2,
	.engine_mask = I915_ENGINE_RCS0,
	.dma_mask_bits = 36,
};

static const struct intel_device_info info_i965gm = {
	.name = "i965GM",
	.platform = I915_PLATFORM_I965GM,
	LEGACY_GEN(4, 40, 4),
	.num_pipes = 2,
	.engine_mask = I915_ENGINE_RCS0,
	.dma_mask_bits = 36,
};

static const struct intel_device_info info_g45 = {
	.name = "G45",
	.platform = I915_PLATFORM_G45,
	LEGACY_GEN(4, 40, 4),
	.num_pipes = 2,
	.flags = I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_VCS0,
	.dma_mask_bits = 36,
};

static const struct intel_device_info info_gm45 = {
	.name = "GM45",
	.platform = I915_PLATFORM_GM45,
	LEGACY_GEN(4, 40, 4),
	.num_pipes = 2,
	.flags = I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_VCS0,
	.dma_mask_bits = 36,
};

static const struct intel_device_info info_ironlake = {
	.name = "Ironlake",
	.platform = I915_PLATFORM_IRONLAKE,
	LEGACY_GEN(5, 50, 5),
	.num_pipes = 2,
	.flags = I915_INFO_HAS_SNOOP | I915_INFO_HAS_PCH,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_VCS0,
	.dma_mask_bits = 36,
};

static const struct intel_device_info info_sandybridge = {
	.name = "Sandy Bridge",
	.platform = I915_PLATFORM_SANDYBRIDGE,
	LEGACY_GEN(6, 60, 6),
	.num_pipes = 2,
	.ppgtt_bits = 31,
	.flags = I915_INFO_HAS_LLC | I915_INFO_HAS_PCH,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_VCS0 | I915_ENGINE_BCS0,
	.dma_mask_bits = 40,
};

static const struct intel_device_info info_ivybridge = {
	.name = "Ivy Bridge",
	.platform = I915_PLATFORM_IVYBRIDGE,
	LEGACY_GEN(7, 70, 7),
	.num_pipes = 3,
	.ppgtt_bits = 31,
	.flags = I915_INFO_HAS_LLC | I915_INFO_HAS_PCH,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_VCS0 | I915_ENGINE_BCS0,
	.dma_mask_bits = 40,
};

static const struct intel_device_info info_valleyview = {
	.name = "Valleyview",
	.platform = I915_PLATFORM_VALLEYVIEW,
	LEGACY_GEN(7, 70, 7),
	.num_pipes = 2,
	.ppgtt_bits = 31,
	.flags = I915_INFO_IS_LP | I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_VCS0 | I915_ENGINE_BCS0,
	.dma_mask_bits = 40,
};

/* The GT of generation 7.5, the DDI display of Broadwell (its DPLLs too:
 * intel_display.c gives Haswell Broadwell's display model). */
static const struct intel_device_info info_haswell = {
	.name = "Haswell",
	.platform = I915_PLATFORM_HASWELL,
	LEGACY_GEN(7, 75, 7),
	.num_pipes = 3,
	.ppgtt_bits = 31,
	.flags = I915_INFO_HAS_DDI | I915_INFO_HAS_LLC | I915_INFO_HAS_PCH,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_VCS0 | I915_ENGINE_BCS0 |
		       I915_ENGINE_VECS0,
	.dma_mask_bits = 39,
};

/* ---- generation 8 ------------------------------------------------------ */

#define GEN8_COMMON                                                        \
	.gen = 8, .gen_x10 = 80, .display_ver = 8, .num_pipes = 3,          \
	.csb_entries = 6, .dpll_model = I915_DPLL_LEGACY, .max_dpll = 3,     \
	.dma_mask_bits = 39

static const struct intel_device_info info_broadwell = {
	.name = "Broadwell",
	.platform = I915_PLATFORM_BROADWELL,
	GEN8_COMMON,
	.ppgtt_bits = 48,
	.flags = I915_INFO_HAS_DDI | I915_INFO_HAS_EXECLISTS |
		 I915_INFO_HAS_LLC | I915_INFO_HAS_FULL_PPGTT |
		 I915_INFO_HAS_PCH | I915_INFO_HAS_DP_MST,
	/* the second video engine is the GT3 parts' (i915_engines_init) */
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VECS0,
};

static const struct intel_device_info info_cherryview = {
	.name = "Cherryview",
	.platform = I915_PLATFORM_CHERRYVIEW,
	GEN8_COMMON,
	.ppgtt_bits = 32,
	.flags = I915_INFO_HAS_EXECLISTS | I915_INFO_IS_LP |
		 I915_INFO_HAS_FULL_PPGTT | I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VECS0,
};

/* ---- generation 9 ------------------------------------------------------ */

#define GEN9_COMMON                                                        \
	.gen = 9, .gen_x10 = 90, .num_pipes = 3, .ppgtt_bits = 48,           \
	.csb_entries = 6, .dma_mask_bits = 39,                              \
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 |                \
		       I915_ENGINE_VCS0 | I915_ENGINE_VECS0
#define GEN9_CORE_DISPLAY .display_ver = 9, .dpll_model = I915_DPLL_SKL, .max_dpll = 4

#define GEN9_CORE_FLAGS                                                    \
	(I915_INFO_HAS_DDI | I915_INFO_HAS_EXECLISTS | I915_INFO_HAS_LLC | \
	 I915_INFO_HAS_GUC | I915_INFO_HAS_DMC | I915_INFO_HAS_FULL_PPGTT | \
	 I915_INFO_HAS_PCH | I915_INFO_HAS_DP_MST)

static const struct intel_device_info info_skylake = {
	.name = "Skylake",
	.platform = I915_PLATFORM_SKYLAKE,
	GEN9_COMMON,
	GEN9_CORE_DISPLAY,
	.flags = GEN9_CORE_FLAGS,
	.dmc_fw = "i915/skl_dmc_ver1_27.bin",
	.guc_fw = "i915/skl_guc_70.1.1.bin",
	.huc_fw = "i915/skl_huc_2.0.0.bin",
};

static const struct intel_device_info info_kabylake = {
	.name = "Kaby Lake",
	.platform = I915_PLATFORM_KABYLAKE,
	GEN9_COMMON,
	GEN9_CORE_DISPLAY,
	.flags = GEN9_CORE_FLAGS,
	.dmc_fw = "i915/kbl_dmc_ver1_04.bin",
	.guc_fw = "i915/kbl_guc_70.1.1.bin",
	.huc_fw = "i915/kbl_huc_4.0.0.bin",
};

static const struct intel_device_info info_coffeelake = {
	.name = "Coffee Lake",
	.platform = I915_PLATFORM_COFFEELAKE,
	GEN9_COMMON,
	GEN9_CORE_DISPLAY,
	.flags = GEN9_CORE_FLAGS,
	.dmc_fw = "i915/kbl_dmc_ver1_04.bin",
	.guc_fw = "i915/kbl_guc_70.1.1.bin",
	.huc_fw = "i915/kbl_huc_4.0.0.bin",
};

static const struct intel_device_info info_cometlake = {
	.name = "Comet Lake",
	.platform = I915_PLATFORM_COMETLAKE,
	GEN9_COMMON,
	GEN9_CORE_DISPLAY,
	.flags = GEN9_CORE_FLAGS,
	.dmc_fw = "i915/kbl_dmc_ver1_04.bin",
	.guc_fw = "i915/cml_guc_70.1.1.bin",
	.huc_fw = "i915/cml_huc_4.0.0.bin",
};

#define GEN9_LP_FLAGS                                                      \
	(I915_INFO_HAS_DDI | I915_INFO_HAS_EXECLISTS | I915_INFO_HAS_GUC | \
	 I915_INFO_HAS_DMC | I915_INFO_IS_LP | I915_INFO_HAS_FULL_PPGTT |  \
	 I915_INFO_HAS_SNOOP | I915_INFO_HAS_DP_MST)

static const struct intel_device_info info_broxton = {
	.name = "Broxton",
	.platform = I915_PLATFORM_BROXTON,
	GEN9_COMMON,
	.display_ver = 9,
	.dpll_model = I915_DPLL_BXT,
	.max_dpll = 3,
	.flags = GEN9_LP_FLAGS,
	.dmc_fw = "i915/bxt_dmc_ver1_07.bin",
	.guc_fw = "i915/bxt_guc_70.1.1.bin",
	.huc_fw = "i915/bxt_huc_2.0.0.bin",
};

static const struct intel_device_info info_geminilake = {
	.name = "Gemini Lake",
	.platform = I915_PLATFORM_GEMINILAKE,
	GEN9_COMMON,
	.display_ver = 10,
	.dpll_model = I915_DPLL_BXT,
	.max_dpll = 3,
	.flags = GEN9_LP_FLAGS,
	.dmc_fw = "i915/glk_dmc_ver1_04.bin",
	.guc_fw = "i915/glk_guc_70.1.1.bin",
	.huc_fw = "i915/glk_huc_4.0.0.bin",
};

/* ---- generation 10 ----------------------------------------------------- */

static const struct intel_device_info info_cannonlake = {
	.name = "Cannon Lake",
	.platform = I915_PLATFORM_CANNONLAKE,
	.gen = 10, .gen_x10 = 100, .display_ver = 10, .num_pipes = 3,
	.ppgtt_bits = 48, .csb_entries = 6, .dpll_model = I915_DPLL_CNL,
	.max_dpll = 3, .dma_mask_bits = 39,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VECS0,
	.flags = GEN9_CORE_FLAGS | I915_INFO_HAS_COMBO_PHY,
	.dmc_fw = "i915/cnl_dmc_ver1_07.bin",
};

/* ---- generation 11 ----------------------------------------------------- */

#define GEN11_COMMON                                                       \
	.gen = 11, .gen_x10 = 110, .display_ver = 11, .num_pipes = 3,       \
	.ppgtt_bits = 48, .csb_entries = 12, .dpll_model = I915_DPLL_ICL,   \
	.max_dpll = 7, .dma_mask_bits = 39

static const struct intel_device_info info_icelake = {
	.name = "Ice Lake",
	.platform = I915_PLATFORM_ICELAKE,
	GEN11_COMMON,
	.flags = GEN9_CORE_FLAGS | I915_INFO_HAS_COMBO_PHY |
		 I915_INFO_HAS_TC_PHY,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VCS2 | I915_ENGINE_VECS0,
	.dmc_fw = "i915/icl_dmc_ver1_09.bin",
	.guc_fw = "i915/icl_guc_70.1.1.bin",
	.huc_fw = "i915/icl_huc_9.0.0.bin",
};

static const struct intel_device_info info_elkhartlake = {
	.name = "Elkhart Lake",
	.platform = I915_PLATFORM_ELKHARTLAKE,
	GEN11_COMMON,
	.flags = GEN9_LP_FLAGS | I915_INFO_HAS_COMBO_PHY | I915_INFO_HAS_PCH,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VECS0,
	.dmc_fw = "i915/icl_dmc_ver1_09.bin",
	.guc_fw = "i915/ehl_guc_70.1.1.bin",
	.huc_fw = "i915/ehl_huc_9.0.0.bin",
};

static const struct intel_device_info info_jasperlake = {
	.name = "Jasper Lake",
	.platform = I915_PLATFORM_JASPERLAKE,
	GEN11_COMMON,
	.flags = GEN9_LP_FLAGS | I915_INFO_HAS_COMBO_PHY | I915_INFO_HAS_PCH,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VECS0,
	.dmc_fw = "i915/icl_dmc_ver1_09.bin",
	/* Jasper Lake loads Elkhart Lake's GuC and HuC */
	.guc_fw = "i915/ehl_guc_70.1.1.bin",
	.huc_fw = "i915/ehl_huc_9.0.0.bin",
};

/* ---- generation 12 ----------------------------------------------------- */

#define GEN12_COMMON                                                       \
	.gen = 12, .gen_x10 = 120, .ppgtt_bits = 48, .csb_entries = 12,     \
	.dpll_model = I915_DPLL_ICL, .max_dpll = 8, .dma_mask_bits = 39

#define GEN12_FLAGS                                                        \
	(I915_INFO_HAS_DDI | I915_INFO_HAS_EXECLISTS | I915_INFO_HAS_LLC | \
	 I915_INFO_HAS_GUC | I915_INFO_HAS_DMC | I915_INFO_HAS_FULL_PPGTT | \
	 I915_INFO_HAS_PCH | I915_INFO_HAS_DP_MST | I915_INFO_HAS_COMBO_PHY)

static const struct intel_device_info info_tigerlake = {
	.name = "Tiger Lake",
	.platform = I915_PLATFORM_TIGERLAKE,
	GEN12_COMMON,
	.display_ver = 12,
	.num_pipes = 4,
	.flags = GEN12_FLAGS | I915_INFO_HAS_TC_PHY,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VCS2 | I915_ENGINE_VECS0,
	.dmc_fw = "i915/tgl_dmc_ver2_12.bin",
	.guc_fw = "i915/tgl_guc_70.1.1.bin",
	.huc_fw = "i915/tgl_huc_7.9.3.bin",
};

static const struct intel_device_info info_rocketlake = {
	.name = "Rocket Lake",
	.platform = I915_PLATFORM_ROCKETLAKE,
	GEN12_COMMON,
	.display_ver = 12,
	.num_pipes = 3,
	.flags = GEN12_FLAGS,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VECS0,
	.dmc_fw = "i915/rkl_dmc_ver2_03.bin",
	.guc_fw = "i915/tgl_guc_70.1.1.bin",
	.huc_fw = "i915/tgl_huc_7.9.3.bin",
};

static const struct intel_device_info info_dg1 = {
	.name = "DG1",
	.platform = I915_PLATFORM_DG1,
	GEN12_COMMON,
	.display_ver = 12,
	.num_pipes = 4,
	.flags = (GEN12_FLAGS & ~(I915_INFO_HAS_LLC | I915_INFO_HAS_PCH)) |
		 I915_INFO_IS_DGFX | I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VCS2 | I915_ENGINE_VECS0,
	.dmc_fw = "i915/dg1_dmc_ver2_02.bin",
	.guc_fw = "i915/dg1_guc_70.bin",
	.huc_fw = "i915/dg1_huc.bin",
};

static const struct intel_device_info info_alderlake_s = {
	.name = "Alder Lake-S",
	.platform = I915_PLATFORM_ALDERLAKE_S,
	GEN12_COMMON,
	.display_ver = 12,
	.num_pipes = 4,
	.flags = GEN12_FLAGS,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VCS2 | I915_ENGINE_VECS0,
	.dmc_fw = "i915/adls_dmc_ver2_01.bin",
	/* Alder Lake-S loads Tiger Lake's GuC and HuC */
	.guc_fw = "i915/tgl_guc_70.bin",
	.huc_fw = "i915/tgl_huc.bin",
};

static const struct intel_device_info info_alderlake_p = {
	.name = "Alder Lake-P",
	.platform = I915_PLATFORM_ALDERLAKE_P,
	GEN12_COMMON,
	.display_ver = 13,
	.num_pipes = 4,
	.flags = GEN12_FLAGS | I915_INFO_HAS_TC_PHY,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VCS2 | I915_ENGINE_VECS0,
	.dmc_fw = "i915/adlp_dmc.bin",
	/* its own GuC, Tiger Lake's HuC */
	.guc_fw = "i915/adlp_guc_70.bin",
	.huc_fw = "i915/tgl_huc.bin",
};

static const struct intel_device_info info_dg2 = {
	.name = "DG2",
	.platform = I915_PLATFORM_DG2,
	.gen = 12, .gen_x10 = 125, .display_ver = 13, .num_pipes = 4,
	.ppgtt_bits = 48, .csb_entries = 12, .dpll_model = I915_DPLL_DG2,
	.max_dpll = 8, .dma_mask_bits = 46,
	.flags = (GEN12_FLAGS & ~(I915_INFO_HAS_LLC | I915_INFO_HAS_PCH |
				   I915_INFO_HAS_COMBO_PHY)) |
		 I915_INFO_IS_DGFX | I915_INFO_GUC_MANDATORY |
		 I915_INFO_HAS_4TILE | I915_INFO_HAS_SNOOP,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VCS2 | I915_ENGINE_VECS0 | I915_ENGINE_VECS1 |
		       I915_ENGINE_CCS0 | I915_ENGINE_CCS1 | I915_ENGINE_CCS2 |
		       I915_ENGINE_CCS3,
	.dmc_fw = "i915/dg2_dmc_ver2_08.bin",
	.guc_fw = "i915/dg2_guc_70.bin",
	.huc_fw = "i915/dg2_huc_gsc.bin",
};

static const struct intel_device_info info_meteorlake = {
	.name = "Meteor Lake",
	.platform = I915_PLATFORM_METEORLAKE,
	.gen = 12, .gen_x10 = 127, .display_ver = 14, .num_pipes = 4,
	.ppgtt_bits = 48, .csb_entries = 12, .dpll_model = I915_DPLL_MTL,
	.max_dpll = 8, .dma_mask_bits = 46,
	.flags = (GEN12_FLAGS & ~(I915_INFO_HAS_LLC | I915_INFO_HAS_COMBO_PHY)) |
		 I915_INFO_GUC_MANDATORY | I915_INFO_HAS_4TILE |
		 I915_INFO_HAS_SNOOP | I915_INFO_HAS_TC_PHY,
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 | I915_ENGINE_VCS0 |
		       I915_ENGINE_VCS2 | I915_ENGINE_VECS0 | I915_ENGINE_CCS0,
	.dmc_fw = "i915/mtl_dmc.bin",
	.guc_fw = "i915/mtl_guc_70.bin",
	.huc_fw = "i915/mtl_huc_gsc.bin",
	.gsc_fw = "i915/mtl_gsc_1.bin",
};

/* ---- Xe2, Xe3 and Xe3P ---------------------------------------------------
 *
 * The GT submits through the GuC as Meteor Lake's does; its IP versions,
 * steppings and media GT come from the GMD_ID registers.  Every display
 * of these is a DDI display with display microcode, Tile4 scanout and MST.
 * The video engines sit on a standalone media GT where the part has one;
 * which of the engines named here exist is the fuses' to say. */

#define XE_DISPLAY_FLAGS                                                   \
	(I915_INFO_HAS_DDI | I915_INFO_HAS_DMC | I915_INFO_HAS_4TILE |      \
	 I915_INFO_HAS_DP_MST)

#define XE2_GT_FLAGS                                                       \
	(I915_INFO_HAS_EXECLISTS | I915_INFO_HAS_GUC |                      \
	 I915_INFO_GUC_MANDATORY | I915_INFO_HAS_FULL_PPGTT |              \
	 I915_INFO_HAS_SNOOP)

#define XE2_COMMON                                                         \
	.ppgtt_bits = 48, .csb_entries = 12, .dpll_model = I915_DPLL_MTL,   \
	.max_dpll = 8, .dma_mask_bits = 46,                                 \
	.engine_mask = I915_ENGINE_RCS0 | I915_ENGINE_BCS0 |                \
		       I915_ENGINE_VCS0 | I915_ENGINE_VCS1 |                \
		       I915_ENGINE_VCS2 | I915_ENGINE_VCS3 |                \
		       I915_ENGINE_VECS0 | I915_ENGINE_VECS1 |              \
		       I915_ENGINE_CCS0 | I915_ENGINE_CCS1 |                \
		       I915_ENGINE_CCS2 | I915_ENGINE_CCS3

/* Lunar Lake: graphics 20.04, media 20.00, display 20.00 */
static const struct intel_device_info info_lunarlake = {
	.name = "Lunar Lake",
	.platform = I915_PLATFORM_LUNARLAKE,
	.gen = 20, .gen_x10 = 200, .display_ver = 20, .num_pipes = 4,
	XE2_COMMON,
	.flags = XE_DISPLAY_FLAGS | I915_INFO_HAS_TC_PHY | I915_INFO_HAS_PCH |
		 XE2_GT_FLAGS,
	.dmc_fw = "i915/xe2lpd_dmc.bin",
	.guc_fw = "xe/lnl_guc_70.bin",
	.huc_fw = "xe/lnl_huc.bin",
	.gsc_fw = "xe/lnl_gsc_1.bin",
};

/* Battlemage: graphics 20.01, media 13.01, display 14.01; discrete, its
 * own memory, no Type-C PHYs and no south display of a PCH; no GSC
 * image to load */
static const struct intel_device_info info_battlemage = {
	.name = "Battlemage",
	.platform = I915_PLATFORM_BATTLEMAGE,
	.gen = 20, .gen_x10 = 201, .display_ver = 14, .num_pipes = 4,
	XE2_COMMON,
	.flags = XE_DISPLAY_FLAGS | I915_INFO_IS_DGFX | XE2_GT_FLAGS,
	.dmc_fw = "i915/bmg_dmc.bin",
	.guc_fw = "xe/bmg_guc_70.bin",
	.huc_fw = "xe/bmg_huc.bin",
};

/* Panther Lake: graphics 30.00/30.01, media 30.00, display 30.00 */
static const struct intel_device_info info_pantherlake = {
	.name = "Panther Lake",
	.platform = I915_PLATFORM_PANTHERLAKE,
	.gen = 30, .gen_x10 = 300, .display_ver = 30, .num_pipes = 4,
	XE2_COMMON,
	.flags = XE_DISPLAY_FLAGS | I915_INFO_HAS_TC_PHY | I915_INFO_HAS_PCH |
		 XE2_GT_FLAGS,
	.dmc_fw = "i915/xe3lpd_dmc.bin",
	.guc_fw = "xe/ptl_guc_70.bin",
	.huc_fw = "xe/ptl_huc.bin",
	.gsc_fw = "xe/ptl_gsc_1.bin",
};

/* Wildcat Lake: Panther Lake's graphics (30.03) and media (30.02) and
 * GuC/HuC/GSC images, three pipes, its own display release (30.02) with
 * its own display microcode */
static const struct intel_device_info info_wildcatlake = {
	.name = "Wildcat Lake",
	.platform = I915_PLATFORM_WILDCATLAKE,
	.gen = 30, .gen_x10 = 300, .display_ver = 30, .num_pipes = 3,
	XE2_COMMON,
	.flags = XE_DISPLAY_FLAGS | I915_INFO_HAS_TC_PHY | I915_INFO_HAS_PCH |
		 XE2_GT_FLAGS,
	.dmc_fw = "i915/xe3lpd_3002_dmc.bin",
	.guc_fw = "xe/ptl_guc_70.bin",
	.huc_fw = "xe/ptl_huc.bin",
	.gsc_fw = "xe/ptl_gsc_1.bin",
};

/* Nova Lake-S: Xe3 graphics (30.05) beside the Xe3P display (35.00) and
 * media (35.00); a GuC image, no HuC or GSC image */
static const struct intel_device_info info_novalake_s = {
	.name = "Nova Lake-S",
	.platform = I915_PLATFORM_NOVALAKE_S,
	.gen = 30, .gen_x10 = 300, .display_ver = 35, .num_pipes = 4,
	XE2_COMMON,
	.flags = XE_DISPLAY_FLAGS | I915_INFO_HAS_TC_PHY | I915_INFO_HAS_PCH |
		 XE2_GT_FLAGS,
	.dmc_fw = "i915/xe3p_lpd_dmc.bin",
	.guc_fw = "xe/nvl_guc_70.bin",
};

/* Nova Lake-P: Xe3P graphics (35.10) and display (35.00).  No GuC image
 * is published for it, and its GT keeps its ring state outside the
 * context image (indirect ring state), which nothing here drives: the
 * display is driven, rendering stays off. */
static const struct intel_device_info info_novalake_p = {
	.name = "Nova Lake-P",
	.platform = I915_PLATFORM_NOVALAKE_P,
	.gen = 35, .gen_x10 = 351, .display_ver = 35, .num_pipes = 4,
	XE2_COMMON,
	.flags = XE_DISPLAY_FLAGS | I915_INFO_HAS_TC_PHY | I915_INFO_HAS_PCH |
		 XE2_GT_FLAGS | I915_INFO_GT_UNSUPPORTED,
	.dmc_fw = "i915/xe3p_lpd_dmc.bin",
};

/* ---- named only: the data-centre parts ----------------------------------- */

/* A part the driver names but does not drive, with its real graphics IP
 * (gen_x10 is the version times ten plus the first digit of the release:
 * 20.01 is 201) and its display IP (0 where it has no display). */
#define NAMED_IP(platform_, name_, gen_, gen_x10_, display_ver_)           \
	.name = name_, .platform = platform_, .gen = gen_,                 \
	.gen_x10 = gen_x10_, .display_ver = display_ver_,                  \
	.flags = I915_INFO_NAME_ONLY

/* Ponte Vecchio: graphics 12.60, compute only */
static const struct intel_device_info info_pontevecchio = {
	NAMED_IP(I915_PLATFORM_PONTEVECCHIO, "Ponte Vecchio", 12, 126, 0),
};

/* Crescent Island: Xe3P compute graphics (35.11), no display */
static const struct intel_device_info info_crescentisland = {
	NAMED_IP(I915_PLATFORM_CRESCENTISLAND, "Crescent Island", 35, 351, 0),
};

/* ---- the table -------------------------------------------------------- */

#define ID(id_, gt_, name_, info_) { .id = id_, .gt = gt_, .name = name_, .info = &info_ }

static const struct i915_pci_id i915_ids[] = {
	/* generation 1 */
	ID(0x7121, 0, "82810", info_i810),
	ID(0x7123, 0, "82810-DC100", info_i810),
	ID(0x7125, 0, "82810E", info_i810),
	ID(0x1132, 0, "82815", info_i815),
	/* generation 2 */
	ID(0x3577, 0, "82830M", info_i830),
	ID(0x2562, 0, "82845G", info_i845g),
	ID(0x3582, 0, "82852/855GM", info_i85x),
	ID(0x358e, 0, "82854GM", info_i85x),
	ID(0x2572, 0, "82865G", info_i865g),
	/* generation 3 */
	ID(0x2582, 0, "82915G", info_i915g),
	ID(0x258a, 0, "E7221G", info_i915g),
	ID(0x2592, 0, "82915GM", info_i915gm),
	ID(0x2772, 0, "82945G", info_i945g),
	ID(0x27a2, 0, "82945GM", info_i945gm),
	ID(0x27ae, 0, "82945GME", info_i945gm),
	ID(0x29b2, 0, "Q35", info_g33),
	ID(0x29c2, 0, "G33", info_g33),
	ID(0x29d2, 0, "Q33", info_g33),
	ID(0xa001, 0, "Pineview", info_pineview),
	ID(0xa011, 0, "Pineview M", info_pineview),
	/* generation 4 */
	ID(0x2972, 0, "946GZ", info_i965g),
	ID(0x2982, 0, "G35", info_i965g),
	ID(0x2992, 0, "Q965", info_i965g),
	ID(0x29a2, 0, "G965", info_i965g),
	ID(0x2a02, 0, "GM965", info_i965gm),
	ID(0x2a12, 0, "GME965", info_i965gm),
	ID(0x2a42, 0, "GM45", info_gm45),
	ID(0x2e02, 0, "Eaglelake", info_g45),
	ID(0x2e12, 0, "Q45", info_g45),
	ID(0x2e22, 0, "G45", info_g45),
	ID(0x2e32, 0, "G41", info_g45),
	ID(0x2e42, 0, "B43", info_g45),
	ID(0x2e92, 0, "B43", info_g45),
	/* generation 5 */
	ID(0x0042, 0, "Ironlake", info_ironlake),
	ID(0x0046, 0, "Ironlake M", info_ironlake),
	/* generation 6 */
	ID(0x0102, 1, "HD Graphics 2000", info_sandybridge),
	ID(0x0112, 2, "HD Graphics 3000", info_sandybridge),
	ID(0x0122, 2, "HD Graphics 3000", info_sandybridge),
	ID(0x0106, 1, "HD Graphics 2000", info_sandybridge),
	ID(0x0116, 2, "HD Graphics 3000", info_sandybridge),
	ID(0x0126, 2, "HD Graphics 3000", info_sandybridge),
	ID(0x010a, 1, "HD Graphics", info_sandybridge),
	/* generation 7 */
	ID(0x0152, 1, "HD Graphics 2500", info_ivybridge),
	ID(0x0162, 2, "HD Graphics 4000", info_ivybridge),
	ID(0x0156, 1, "HD Graphics 2500", info_ivybridge),
	ID(0x0166, 2, "HD Graphics 4000", info_ivybridge),
	ID(0x015a, 1, "HD Graphics", info_ivybridge),
	ID(0x016a, 2, "HD Graphics P4000", info_ivybridge),
	ID(0x0f30, 0, "Valleyview", info_valleyview),
	ID(0x0f31, 0, "Valleyview", info_valleyview),
	ID(0x0f32, 0, "Valleyview", info_valleyview),
	ID(0x0f33, 0, "Valleyview", info_valleyview),
	ID(0x0155, 0, "Valleyview", info_valleyview),
	ID(0x0157, 0, "Valleyview", info_valleyview),
	ID(0x0402, 1, "HD Graphics", info_haswell),
	ID(0x0412, 2, "HD Graphics 4600", info_haswell),
	ID(0x0422, 3, "HD Graphics 5000", info_haswell),
	ID(0x0406, 1, "HD Graphics", info_haswell),
	ID(0x0416, 2, "HD Graphics 4600", info_haswell),
	ID(0x0426, 3, "HD Graphics 5000", info_haswell),
	ID(0x040a, 1, "HD Graphics", info_haswell),
	ID(0x041a, 2, "HD Graphics P4600", info_haswell),
	ID(0x042a, 3, "HD Graphics", info_haswell),
	ID(0x040b, 1, "HD Graphics", info_haswell),
	ID(0x041b, 2, "HD Graphics", info_haswell),
	ID(0x042b, 3, "HD Graphics", info_haswell),
	ID(0x040e, 1, "HD Graphics", info_haswell),
	ID(0x041e, 2, "HD Graphics 4400", info_haswell),
	ID(0x042e, 3, "HD Graphics", info_haswell),
	ID(0x0a02, 1, "HD Graphics", info_haswell),
	ID(0x0a12, 2, "HD Graphics", info_haswell),
	ID(0x0a22, 3, "HD Graphics", info_haswell),
	ID(0x0a06, 1, "HD Graphics", info_haswell),
	ID(0x0a16, 2, "HD Graphics 4400", info_haswell),
	ID(0x0a26, 3, "HD Graphics 5000", info_haswell),
	ID(0x0a0a, 1, "HD Graphics", info_haswell),
	ID(0x0a1a, 2, "HD Graphics", info_haswell),
	ID(0x0a2a, 3, "HD Graphics", info_haswell),
	ID(0x0a0b, 1, "HD Graphics", info_haswell),
	ID(0x0a1b, 2, "HD Graphics", info_haswell),
	ID(0x0a2b, 3, "HD Graphics", info_haswell),
	ID(0x0a0e, 1, "HD Graphics", info_haswell),
	ID(0x0a1e, 2, "HD Graphics 4200", info_haswell),
	ID(0x0a2e, 3, "Iris Graphics 5100", info_haswell),
	ID(0x0d02, 1, "HD Graphics", info_haswell),
	ID(0x0d12, 2, "HD Graphics 4600", info_haswell),
	ID(0x0d22, 3, "Iris Pro Graphics 5200", info_haswell),
	ID(0x0d06, 1, "HD Graphics", info_haswell),
	ID(0x0d16, 2, "HD Graphics 4600", info_haswell),
	ID(0x0d26, 3, "Iris Pro Graphics 5200", info_haswell),
	ID(0x0d0a, 1, "HD Graphics", info_haswell),
	ID(0x0d1a, 2, "HD Graphics", info_haswell),
	ID(0x0d2a, 3, "Iris Pro Graphics", info_haswell),
	ID(0x0d0b, 1, "HD Graphics", info_haswell),
	ID(0x0d1b, 2, "HD Graphics", info_haswell),
	ID(0x0d2b, 3, "Iris Pro Graphics", info_haswell),
	ID(0x0d0e, 1, "HD Graphics", info_haswell),
	ID(0x0d1e, 2, "HD Graphics", info_haswell),
	ID(0x0d2e, 3, "Iris Pro Graphics", info_haswell),
	ID(0x0c02, 1, "HD Graphics", info_haswell),
	ID(0x0c12, 2, "HD Graphics", info_haswell),
	ID(0x0c22, 3, "HD Graphics", info_haswell),
	ID(0x0c06, 1, "HD Graphics", info_haswell),
	ID(0x0c16, 2, "HD Graphics", info_haswell),
	ID(0x0c26, 3, "HD Graphics", info_haswell),
	ID(0x0c0a, 1, "HD Graphics", info_haswell),
	ID(0x0c1a, 2, "HD Graphics", info_haswell),
	ID(0x0c2a, 3, "HD Graphics", info_haswell),
	ID(0x0c0b, 1, "HD Graphics", info_haswell),
	ID(0x0c1b, 2, "HD Graphics", info_haswell),
	ID(0x0c2b, 3, "HD Graphics", info_haswell),
	ID(0x0c0e, 1, "HD Graphics", info_haswell),
	ID(0x0c1e, 2, "HD Graphics", info_haswell),
	ID(0x0c2e, 3, "HD Graphics", info_haswell),
	/* generation 8: Broadwell */
	ID(0x1602, 1, "HD Graphics", info_broadwell),
	ID(0x1606, 1, "HD Graphics", info_broadwell),
	ID(0x160a, 1, "HD Graphics", info_broadwell),
	ID(0x160b, 1, "HD Graphics", info_broadwell),
	ID(0x160d, 1, "HD Graphics", info_broadwell),
	ID(0x160e, 1, "HD Graphics", info_broadwell),
	ID(0x1612, 2, "HD Graphics 5600", info_broadwell),
	ID(0x1616, 2, "HD Graphics 5500", info_broadwell),
	ID(0x161a, 2, "HD Graphics P5700", info_broadwell),
	ID(0x161b, 2, "HD Graphics", info_broadwell),
	ID(0x161d, 2, "HD Graphics", info_broadwell),
	ID(0x161e, 2, "HD Graphics 5300", info_broadwell),
	ID(0x1622, 3, "Iris Pro Graphics 6200", info_broadwell),
	ID(0x1626, 3, "HD Graphics 6000", info_broadwell),
	ID(0x162a, 3, "Iris Pro Graphics P6300", info_broadwell),
	ID(0x162b, 3, "Iris Graphics 6100", info_broadwell),
	ID(0x162d, 3, "Iris Pro Graphics", info_broadwell),
	ID(0x162e, 3, "Iris Graphics", info_broadwell),
	ID(0x1632, 3, "HD Graphics", info_broadwell),
	ID(0x1636, 3, "HD Graphics", info_broadwell),
	ID(0x163a, 3, "HD Graphics", info_broadwell),
	ID(0x163b, 3, "HD Graphics", info_broadwell),
	ID(0x163d, 3, "HD Graphics", info_broadwell),
	ID(0x163e, 3, "HD Graphics", info_broadwell),
	ID(0x22b0, 0, "HD Graphics", info_cherryview),
	ID(0x22b1, 0, "HD Graphics", info_cherryview),
	ID(0x22b2, 0, "HD Graphics", info_cherryview),
	ID(0x22b3, 0, "HD Graphics", info_cherryview),
	/* generation 9: Skylake */
	ID(0x1902, 1, "HD Graphics 510", info_skylake),
	ID(0x1906, 1, "HD Graphics 510", info_skylake),
	ID(0x190a, 1, "HD Graphics", info_skylake),
	ID(0x190b, 1, "HD Graphics 510", info_skylake),
	ID(0x190e, 1, "HD Graphics", info_skylake),
	ID(0x1912, 2, "HD Graphics 530", info_skylake),
	ID(0x1913, 2, "HD Graphics", info_skylake),
	ID(0x1915, 2, "HD Graphics", info_skylake),
	ID(0x1916, 2, "HD Graphics 520", info_skylake),
	ID(0x1917, 2, "HD Graphics", info_skylake),
	ID(0x191a, 2, "HD Graphics", info_skylake),
	ID(0x191b, 2, "HD Graphics 530", info_skylake),
	ID(0x191d, 2, "HD Graphics P530", info_skylake),
	ID(0x191e, 2, "HD Graphics 515", info_skylake),
	ID(0x1921, 2, "HD Graphics 520", info_skylake),
	ID(0x1923, 3, "HD Graphics 535", info_skylake),
	ID(0x1926, 3, "Iris Graphics 540", info_skylake),
	ID(0x1927, 3, "Iris Graphics 550", info_skylake),
	ID(0x192a, 4, "Iris Pro Graphics", info_skylake),
	ID(0x192b, 3, "Iris Graphics 555", info_skylake),
	ID(0x192d, 3, "Iris Graphics P555", info_skylake),
	ID(0x1932, 4, "Iris Pro Graphics 580", info_skylake),
	ID(0x193a, 4, "Iris Pro Graphics P580", info_skylake),
	ID(0x193b, 4, "Iris Pro Graphics 580", info_skylake),
	ID(0x193d, 4, "Iris Pro Graphics P580", info_skylake),
	/* generation 9: Broxton / Gemini Lake */
	ID(0x0a84, 0, "HD Graphics", info_broxton),
	ID(0x1a84, 0, "HD Graphics", info_broxton),
	ID(0x1a85, 0, "HD Graphics", info_broxton),
	ID(0x5a84, 0, "HD Graphics 505", info_broxton),
	ID(0x5a85, 0, "HD Graphics 500", info_broxton),
	ID(0x3184, 0, "UHD Graphics 605", info_geminilake),
	ID(0x3185, 0, "UHD Graphics 600", info_geminilake),
	/* generation 9: Kaby Lake */
	ID(0x5902, 1, "HD Graphics 610", info_kabylake),
	ID(0x5906, 1, "HD Graphics 610", info_kabylake),
	ID(0x5908, 1, "HD Graphics", info_kabylake),
	ID(0x590a, 1, "HD Graphics", info_kabylake),
	ID(0x590b, 1, "HD Graphics 610", info_kabylake),
	ID(0x590e, 1, "HD Graphics", info_kabylake),
	ID(0x5912, 2, "HD Graphics 630", info_kabylake),
	ID(0x5913, 2, "HD Graphics", info_kabylake),
	ID(0x5915, 2, "HD Graphics", info_kabylake),
	ID(0x5916, 2, "HD Graphics 620", info_kabylake),
	ID(0x5917, 2, "UHD Graphics 620", info_kabylake),
	ID(0x591a, 2, "HD Graphics P630", info_kabylake),
	ID(0x591b, 2, "HD Graphics 630", info_kabylake),
	ID(0x591c, 2, "UHD Graphics 615", info_kabylake),
	ID(0x591d, 2, "HD Graphics P630", info_kabylake),
	ID(0x591e, 2, "HD Graphics 615", info_kabylake),
	ID(0x5921, 2, "HD Graphics 620", info_kabylake),
	ID(0x5923, 3, "HD Graphics 635", info_kabylake),
	ID(0x5926, 3, "Iris Plus Graphics 640", info_kabylake),
	ID(0x5927, 3, "Iris Plus Graphics 650", info_kabylake),
	ID(0x593b, 4, "HD Graphics", info_kabylake),
	ID(0x87c0, 2, "UHD Graphics 617", info_kabylake),
	/* generation 9: Coffee Lake / Whiskey Lake / Amber Lake */
	ID(0x3e90, 1, "UHD Graphics 610", info_coffeelake),
	ID(0x3e93, 1, "UHD Graphics 610", info_coffeelake),
	ID(0x3e99, 1, "UHD Graphics", info_coffeelake),
	ID(0x3e9c, 1, "UHD Graphics", info_coffeelake),
	ID(0x3e91, 2, "UHD Graphics 630", info_coffeelake),
	ID(0x3e92, 2, "UHD Graphics 630", info_coffeelake),
	ID(0x3e94, 2, "UHD Graphics P630", info_coffeelake),
	ID(0x3e96, 2, "UHD Graphics P630", info_coffeelake),
	ID(0x3e98, 2, "UHD Graphics 630", info_coffeelake),
	ID(0x3e9a, 2, "UHD Graphics P630", info_coffeelake),
	ID(0x3e9b, 2, "UHD Graphics 630", info_coffeelake),
	ID(0x3ea9, 2, "UHD Graphics 620", info_coffeelake),
	ID(0x3ea0, 2, "UHD Graphics 620", info_coffeelake),
	ID(0x3ea1, 1, "UHD Graphics", info_coffeelake),
	ID(0x3ea2, 3, "Iris Plus Graphics", info_coffeelake),
	ID(0x3ea3, 2, "UHD Graphics", info_coffeelake),
	ID(0x3ea4, 1, "UHD Graphics", info_coffeelake),
	ID(0x3ea5, 3, "Iris Plus Graphics 655", info_coffeelake),
	ID(0x3ea6, 3, "Iris Plus Graphics", info_coffeelake),
	ID(0x3ea7, 3, "Iris Plus Graphics", info_coffeelake),
	ID(0x3ea8, 3, "Iris Plus Graphics 655", info_coffeelake),
	ID(0x87ca, 2, "UHD Graphics 617", info_coffeelake),
	/* generation 9: Comet Lake */
	ID(0x9b21, 1, "UHD Graphics", info_cometlake),
	ID(0x9ba0, 1, "UHD Graphics", info_cometlake),
	ID(0x9ba2, 1, "UHD Graphics", info_cometlake),
	ID(0x9ba4, 1, "UHD Graphics", info_cometlake),
	ID(0x9ba5, 1, "UHD Graphics", info_cometlake),
	ID(0x9ba8, 1, "UHD Graphics 610", info_cometlake),
	ID(0x9baa, 1, "UHD Graphics", info_cometlake),
	ID(0x9bab, 1, "UHD Graphics", info_cometlake),
	ID(0x9bac, 1, "UHD Graphics", info_cometlake),
	ID(0x9b41, 2, "UHD Graphics", info_cometlake),
	ID(0x9bc0, 2, "UHD Graphics", info_cometlake),
	ID(0x9bc2, 2, "UHD Graphics", info_cometlake),
	ID(0x9bc4, 2, "UHD Graphics", info_cometlake),
	ID(0x9bc5, 2, "UHD Graphics 630", info_cometlake),
	ID(0x9bc6, 2, "UHD Graphics P630", info_cometlake),
	ID(0x9bc8, 2, "UHD Graphics 630", info_cometlake),
	ID(0x9bca, 2, "UHD Graphics", info_cometlake),
	ID(0x9bcb, 2, "UHD Graphics", info_cometlake),
	ID(0x9bcc, 2, "UHD Graphics", info_cometlake),
	ID(0x9be6, 2, "UHD Graphics P630", info_cometlake),
	ID(0x9bf6, 2, "UHD Graphics P630", info_cometlake),
	/* generation 10: Cannon Lake */
	ID(0x5a40, 1, "UHD Graphics", info_cannonlake),
	ID(0x5a41, 1, "UHD Graphics", info_cannonlake),
	ID(0x5a42, 1, "UHD Graphics", info_cannonlake),
	ID(0x5a44, 1, "UHD Graphics", info_cannonlake),
	ID(0x5a49, 1, "UHD Graphics", info_cannonlake),
	ID(0x5a4a, 1, "UHD Graphics", info_cannonlake),
	ID(0x5a4c, 1, "UHD Graphics", info_cannonlake),
	ID(0x5a50, 2, "UHD Graphics", info_cannonlake),
	ID(0x5a51, 2, "UHD Graphics", info_cannonlake),
	ID(0x5a52, 2, "UHD Graphics", info_cannonlake),
	ID(0x5a54, 2, "UHD Graphics", info_cannonlake),
	ID(0x5a59, 2, "UHD Graphics", info_cannonlake),
	ID(0x5a5a, 2, "UHD Graphics", info_cannonlake),
	ID(0x5a5c, 2, "UHD Graphics", info_cannonlake),
	/* generation 11: Ice Lake */
	ID(0x8a50, 2, "Iris Plus Graphics", info_icelake),
	ID(0x8a51, 2, "Iris Plus Graphics", info_icelake),
	ID(0x8a52, 2, "Iris Plus Graphics", info_icelake),
	ID(0x8a53, 2, "Iris Plus Graphics", info_icelake),
	ID(0x8a54, 1, "UHD Graphics", info_icelake),
	ID(0x8a56, 1, "UHD Graphics", info_icelake),
	ID(0x8a57, 1, "UHD Graphics", info_icelake),
	ID(0x8a58, 1, "UHD Graphics", info_icelake),
	ID(0x8a59, 1, "UHD Graphics", info_icelake),
	ID(0x8a5a, 2, "Iris Plus Graphics", info_icelake),
	ID(0x8a5b, 1, "UHD Graphics", info_icelake),
	ID(0x8a5c, 2, "Iris Plus Graphics", info_icelake),
	ID(0x8a5d, 1, "UHD Graphics", info_icelake),
	ID(0x8a70, 1, "UHD Graphics", info_icelake),
	ID(0x8a71, 1, "UHD Graphics", info_icelake),
	/* generation 11: Elkhart Lake / Jasper Lake */
	ID(0x4500, 0, "UHD Graphics", info_elkhartlake),
	ID(0x4541, 0, "UHD Graphics", info_elkhartlake),
	ID(0x4551, 0, "UHD Graphics", info_elkhartlake),
	ID(0x4555, 0, "UHD Graphics", info_elkhartlake),
	ID(0x4557, 0, "UHD Graphics", info_elkhartlake),
	ID(0x4570, 0, "UHD Graphics", info_elkhartlake),
	ID(0x4571, 0, "UHD Graphics", info_elkhartlake),
	ID(0x4e51, 0, "UHD Graphics", info_jasperlake),
	ID(0x4e55, 0, "UHD Graphics", info_jasperlake),
	ID(0x4e57, 0, "UHD Graphics", info_jasperlake),
	ID(0x4e61, 0, "UHD Graphics", info_jasperlake),
	ID(0x4e71, 0, "UHD Graphics", info_jasperlake),
	/* generation 12: Tiger Lake */
	ID(0x9a60, 1, "UHD Graphics", info_tigerlake),
	ID(0x9a68, 1, "UHD Graphics", info_tigerlake),
	ID(0x9a70, 1, "UHD Graphics", info_tigerlake),
	ID(0x9a40, 2, "Iris Xe Graphics", info_tigerlake),
	ID(0x9a49, 2, "Iris Xe Graphics", info_tigerlake),
	ID(0x9a59, 2, "Iris Xe Graphics", info_tigerlake),
	ID(0x9a78, 2, "UHD Graphics", info_tigerlake),
	ID(0x9ac0, 2, "Iris Xe Graphics", info_tigerlake),
	ID(0x9ac9, 2, "Iris Xe Graphics", info_tigerlake),
	ID(0x9ad9, 2, "Iris Xe Graphics", info_tigerlake),
	ID(0x9af8, 2, "Iris Xe Graphics", info_tigerlake),
	/* generation 12: Rocket Lake */
	ID(0x4c80, 1, "UHD Graphics 730", info_rocketlake),
	ID(0x4c8a, 2, "UHD Graphics 750", info_rocketlake),
	ID(0x4c8b, 2, "UHD Graphics 750", info_rocketlake),
	ID(0x4c8c, 1, "UHD Graphics 730", info_rocketlake),
	ID(0x4c90, 2, "UHD Graphics P750", info_rocketlake),
	ID(0x4c9a, 2, "UHD Graphics P750", info_rocketlake),
	/* generation 12: DG1 */
	ID(0x4905, 0, "Iris Xe MAX Graphics", info_dg1),
	ID(0x4906, 0, "Iris Xe MAX Graphics", info_dg1),
	ID(0x4907, 0, "Iris Xe MAX Graphics", info_dg1),
	ID(0x4908, 0, "Iris Xe Graphics", info_dg1),
	ID(0x4909, 0, "Iris Xe Graphics", info_dg1),
	/* generation 12: Alder Lake-S / Raptor Lake-S */
	ID(0x4680, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0x4682, 1, "UHD Graphics 730", info_alderlake_s),
	ID(0x4688, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0x468a, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0x468b, 1, "UHD Graphics 710", info_alderlake_s),
	ID(0x4690, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0x4692, 1, "UHD Graphics 730", info_alderlake_s),
	ID(0x4693, 1, "UHD Graphics 710", info_alderlake_s),
	ID(0xa780, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0xa781, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0xa782, 1, "UHD Graphics 730", info_alderlake_s),
	ID(0xa783, 1, "UHD Graphics 710", info_alderlake_s),
	ID(0xa788, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0xa789, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0xa78a, 1, "UHD Graphics 770", info_alderlake_s),
	ID(0xa78b, 1, "UHD Graphics 770", info_alderlake_s),
	/* generation 12: Alder Lake-P / -N, Raptor Lake-P */
	ID(0x46a0, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46a1, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46a2, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46a3, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46a6, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46a8, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46aa, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x462a, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x4626, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x4628, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46b0, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46b1, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46b2, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46b3, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46c0, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46c1, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46c2, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46c3, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0x46d0, 1, "UHD Graphics", info_alderlake_p),
	ID(0x46d1, 1, "UHD Graphics", info_alderlake_p),
	ID(0x46d2, 1, "UHD Graphics", info_alderlake_p),
	ID(0x46d3, 1, "UHD Graphics", info_alderlake_p),
	ID(0x46d4, 1, "UHD Graphics", info_alderlake_p),
	ID(0xa720, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa721, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa7a0, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa7a1, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa7a8, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa7a9, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa7aa, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa7ab, 2, "Iris Xe Graphics", info_alderlake_p),
	ID(0xa7ac, 1, "UHD Graphics", info_alderlake_p),
	ID(0xa7ad, 1, "UHD Graphics", info_alderlake_p),
	/* generation 12.5: DG2 / Arc Alchemist */
	ID(0x5690, 0, "Arc A770M", info_dg2),
	ID(0x5691, 0, "Arc A730M", info_dg2),
	ID(0x5692, 0, "Arc A550M", info_dg2),
	ID(0x56a0, 0, "Arc A770", info_dg2),
	ID(0x56a1, 0, "Arc A750", info_dg2),
	ID(0x56a2, 0, "Arc A580", info_dg2),
	ID(0x56be, 0, "Arc Graphics", info_dg2),
	ID(0x56bf, 0, "Arc Graphics", info_dg2),
	ID(0x5693, 0, "Arc A370M", info_dg2),
	ID(0x5694, 0, "Arc A350M", info_dg2),
	ID(0x5695, 0, "Arc Graphics", info_dg2),
	ID(0x56a5, 0, "Arc A380", info_dg2),
	ID(0x56a6, 0, "Arc A310", info_dg2),
	ID(0x56b0, 0, "Arc Pro A30M", info_dg2),
	ID(0x56b1, 0, "Arc Pro A40/A50", info_dg2),
	ID(0x56ba, 0, "Arc Graphics", info_dg2),
	ID(0x56bb, 0, "Arc Graphics", info_dg2),
	ID(0x56bc, 0, "Arc Graphics", info_dg2),
	ID(0x56bd, 0, "Arc Graphics", info_dg2),
	ID(0x5696, 0, "Arc A570M", info_dg2),
	ID(0x5697, 0, "Arc A530M", info_dg2),
	ID(0x56a3, 0, "Arc Graphics", info_dg2),
	ID(0x56a4, 0, "Arc Graphics", info_dg2),
	ID(0x56b2, 0, "Arc Pro A60M", info_dg2),
	ID(0x56b3, 0, "Arc Pro A60", info_dg2),
	ID(0x56c0, 0, "Data Center GPU Flex 170", info_dg2),
	ID(0x56c1, 0, "Data Center GPU Flex 140", info_dg2),
	ID(0x56c2, 0, "Data Center GPU Flex", info_dg2),
	/* generation 12.7: Meteor Lake / Arrow Lake */
	ID(0x7d40, 0, "Arc Graphics", info_meteorlake),
	ID(0x7d45, 0, "Graphics", info_meteorlake),
	ID(0x7d55, 0, "Arc Graphics", info_meteorlake),
	ID(0x7d60, 0, "Graphics", info_meteorlake),
	ID(0x7dd5, 0, "Arc Graphics", info_meteorlake),
	ID(0x7d41, 0, "Graphics", info_meteorlake),
	ID(0x7d51, 0, "Arc Graphics", info_meteorlake),
	ID(0x7d67, 0, "Graphics", info_meteorlake),
	ID(0x7dd1, 0, "Arc Graphics", info_meteorlake),
	ID(0xb640, 0, "Graphics", info_meteorlake),
	/* generation 12.6: Ponte Vecchio -- named only */
	ID(0x0b69, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0b6e, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0bd4, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0bd5, 0, "Data Center GPU Max 1550", info_pontevecchio),
	ID(0x0bd6, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0bd7, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0bd8, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0bd9, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0bda, 0, "Data Center GPU Max 1100", info_pontevecchio),
	ID(0x0bdb, 0, "Data Center GPU Max 1100", info_pontevecchio),
	ID(0x0be0, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0be1, 0, "Data Center GPU Max", info_pontevecchio),
	ID(0x0be5, 0, "Data Center GPU Max", info_pontevecchio),
	/* Xe2: Lunar Lake / Battlemage; Xe3: Panther Lake / Wildcat Lake;
	 * Xe3/Xe3P: Nova Lake; Xe3P: Crescent Island -- named only */
	ID(0x6420, 0, "Arc Graphics", info_lunarlake),
	ID(0x64a0, 0, "Arc Graphics 140V", info_lunarlake),
	ID(0x64b0, 0, "Arc Graphics 130V", info_lunarlake),
	ID(0xe202, 0, "Arc Graphics", info_battlemage),
	ID(0xe209, 0, "Arc Graphics", info_battlemage),
	ID(0xe20b, 0, "Arc B580", info_battlemage),
	ID(0xe20c, 0, "Arc B570", info_battlemage),
	ID(0xe20d, 0, "Arc Graphics", info_battlemage),
	ID(0xe210, 0, "Arc Graphics", info_battlemage),
	ID(0xe211, 0, "Arc Graphics", info_battlemage),
	ID(0xe212, 0, "Arc Graphics", info_battlemage),
	ID(0xe216, 0, "Arc Graphics", info_battlemage),
	ID(0xe220, 0, "Arc Graphics", info_battlemage),
	ID(0xe221, 0, "Arc Graphics", info_battlemage),
	ID(0xe222, 0, "Arc Graphics", info_battlemage),
	ID(0xe223, 0, "Arc Graphics", info_battlemage),
	ID(0xb080, 0, "Arc Graphics", info_pantherlake),
	ID(0xb081, 0, "Arc Graphics", info_pantherlake),
	ID(0xb082, 0, "Arc Graphics", info_pantherlake),
	ID(0xb083, 0, "Arc Graphics", info_pantherlake),
	ID(0xb084, 0, "Graphics", info_pantherlake),
	ID(0xb085, 0, "Graphics", info_pantherlake),
	ID(0xb086, 0, "Graphics", info_pantherlake),
	ID(0xb087, 0, "Graphics", info_pantherlake),
	ID(0xb08f, 0, "Arc Graphics", info_pantherlake),
	ID(0xb090, 0, "Arc Graphics", info_pantherlake),
	ID(0xb0a0, 0, "Arc Graphics", info_pantherlake),
	ID(0xb0b0, 0, "Arc Graphics", info_pantherlake),
	ID(0xfd80, 0, "Graphics", info_wildcatlake),
	ID(0xfd81, 0, "Graphics", info_wildcatlake),
	ID(0xd740, 0, "Graphics", info_novalake_s),
	ID(0xd741, 0, "Graphics", info_novalake_s),
	ID(0xd742, 0, "Graphics", info_novalake_s),
	ID(0xd743, 0, "Graphics", info_novalake_s),
	ID(0xd745, 0, "Graphics", info_novalake_s),
	ID(0xd74a, 0, "Graphics", info_novalake_s),
	ID(0xd74b, 0, "Graphics", info_novalake_s),
	ID(0xd750, 0, "Graphics", info_novalake_p),
	ID(0xd751, 0, "Graphics", info_novalake_p),
	ID(0xd752, 0, "Graphics", info_novalake_p),
	ID(0xd753, 0, "Graphics", info_novalake_p),
	ID(0xd754, 0, "Graphics", info_novalake_p),
	ID(0xd755, 0, "Graphics", info_novalake_p),
	ID(0xd756, 0, "Graphics", info_novalake_p),
	ID(0xd757, 0, "Graphics", info_novalake_p),
	ID(0xd75f, 0, "Graphics", info_novalake_p),
	ID(0x674c, 0, "Data Center GPU", info_crescentisland),
	ID(0x674d, 0, "Data Center GPU", info_crescentisland),
	ID(0x674e, 0, "Data Center GPU", info_crescentisland),
	ID(0x674f, 0, "Data Center GPU", info_crescentisland),
	ID(0x6750, 0, "Data Center GPU", info_crescentisland),
};

const struct i915_pci_id *i915_pci_lookup(uint16_t device_id)
{
	for (unsigned i = 0; i < sizeof(i915_ids) / sizeof(i915_ids[0]); i++)
		if (i915_ids[i].id == device_id)
			return &i915_ids[i];
	return (const struct i915_pci_id *)0;
}
