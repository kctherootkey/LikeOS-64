// LikeOS -- what kind of Intel graphics device this is.
//
// Every device id in the PCI table points at one of these descriptors:
// the generation and the feature flags the driver gates its code paths
// on.  Nothing in the driver tests a device id or a platform name at run
// time; it asks the descriptor.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_DEVICE_INFO_H
#define KERNEL_DEV_GPU_I915_DEVICE_INFO_H

#include <kernel/uapi/types.h>

enum i915_platform {
	I915_PLATFORM_UNKNOWN = 0,
	/* generation 2 */
	I915_PLATFORM_I830,
	I915_PLATFORM_I845G,
	I915_PLATFORM_I85X,
	I915_PLATFORM_I865G,
	/* generation 3 */
	I915_PLATFORM_I915G,
	I915_PLATFORM_I915GM,
	I915_PLATFORM_I945G,
	I915_PLATFORM_I945GM,
	I915_PLATFORM_G33,
	I915_PLATFORM_PINEVIEW,
	/* generation 4 */
	I915_PLATFORM_I965G,
	I915_PLATFORM_I965GM,
	I915_PLATFORM_G45,
	I915_PLATFORM_GM45,
	/* generation 5 */
	I915_PLATFORM_IRONLAKE,
	/* generation 6 */
	I915_PLATFORM_SANDYBRIDGE,
	/* generation 7 */
	I915_PLATFORM_IVYBRIDGE,
	I915_PLATFORM_VALLEYVIEW,
	I915_PLATFORM_HASWELL,
	/* generation 8 */
	I915_PLATFORM_BROADWELL,
	I915_PLATFORM_CHERRYVIEW,
	/* generation 9 */
	I915_PLATFORM_SKYLAKE,
	I915_PLATFORM_BROXTON,
	I915_PLATFORM_KABYLAKE,
	I915_PLATFORM_GEMINILAKE,
	I915_PLATFORM_COFFEELAKE,
	I915_PLATFORM_COMETLAKE,
	/* generation 10 */
	I915_PLATFORM_CANNONLAKE,
	/* generation 11 */
	I915_PLATFORM_ICELAKE,
	I915_PLATFORM_ELKHARTLAKE,
	I915_PLATFORM_JASPERLAKE,
	/* generation 12 */
	I915_PLATFORM_TIGERLAKE,
	I915_PLATFORM_ROCKETLAKE,
	I915_PLATFORM_DG1,
	I915_PLATFORM_ALDERLAKE_S,
	I915_PLATFORM_ALDERLAKE_P,
	I915_PLATFORM_DG2,
	I915_PLATFORM_METEORLAKE,
	/* Xe2 / Xe3 / Xe3P */
	I915_PLATFORM_LUNARLAKE,
	I915_PLATFORM_BATTLEMAGE,
	I915_PLATFORM_PANTHERLAKE,
	I915_PLATFORM_WILDCATLAKE,
	I915_PLATFORM_NOVALAKE_S,
	I915_PLATFORM_NOVALAKE_P,
	/* data-centre compute parts, no display (named, not driven) */
	I915_PLATFORM_PONTEVECCHIO,
	I915_PLATFORM_CRESCENTISLAND,
	/* generation 1 (named, not driven) */
	I915_PLATFORM_I810,
	I915_PLATFORM_I815,
	I915_PLATFORM_COUNT
};

/* Feature flags.  A driven platform has the code paths its flags name. */
#define I915_INFO_NAME_ONLY (1u << 0) /* identify, log, do not drive */
#define I915_INFO_HAS_DDI (1u << 1) /* DDI-style outputs (Haswell+) */
#define I915_INFO_HAS_EXECLISTS (1u << 2) /* execution lists (Gen8+) */
#define I915_INFO_HAS_LLC (1u << 3) /* a last-level cache shared with the CPU */
#define I915_INFO_HAS_GUC (1u << 4) /* GuC firmware can be loaded */
#define I915_INFO_GUC_MANDATORY (1u << 5) /* submission only through GuC */
#define I915_INFO_HAS_DMC (1u << 6) /* display microcode (DMC) */
#define I915_INFO_IS_LP (1u << 7) /* low-power ("Atom") derivative */
#define I915_INFO_HAS_TC_PHY (1u << 8) /* Type-C PHYs on some DDIs */
#define I915_INFO_HAS_COMBO_PHY (1u << 9) /* combo PHYs (Gen11+) */
#define I915_INFO_IS_DGFX (1u << 10) /* discrete: own memory */
#define I915_INFO_HAS_FULL_PPGTT (1u << 11) /* per-context address spaces */
#define I915_INFO_HAS_4TILE (1u << 12) /* Tile4 scanout/render tiling */
#define I915_INFO_HAS_PSF_GV (1u << 13)
#define I915_INFO_HAS_SNOOP (1u << 14) /* no LLC but snooped system memory */
#define I915_INFO_HAS_PCH (1u << 15) /* south display in a PCH */
#define I915_INFO_HAS_DP_MST (1u << 16)
/* The display is driven, the GT is not: no firmware or no support for
 * its GT yet, rendering stays off (a "display only" device). */
#define I915_INFO_GT_UNSUPPORTED (1u << 17)

/* DPLL models the display code chooses between. */
enum i915_dpll_model {
	I915_DPLL_NONE = 0,
	I915_DPLL_LEGACY, /* pre-Gen9: per-pipe PLLs, Haswell/Broadwell WRPLL/SPLL */
	I915_DPLL_SKL, /* DPLL0..3, LCPLL/WRPLL dividers */
	I915_DPLL_BXT, /* per-port PHY PLLs */
	I915_DPLL_CNL, /* combo PLLs, DCO */
	I915_DPLL_ICL, /* combo + MG/DKL Type-C + TBT */
	I915_DPLL_DG2, /* per-PHY snps PLLs */
	I915_DPLL_MTL, /* C10/C20 PHY PLLs */
};

/* Engine instances the platform has (bit = engine id, see i915_engine.h). */
#define I915_ENGINE_RCS0 (1u << 0)
#define I915_ENGINE_BCS0 (1u << 1)
#define I915_ENGINE_VCS0 (1u << 2)
#define I915_ENGINE_VCS1 (1u << 3)
#define I915_ENGINE_VCS2 (1u << 4)
#define I915_ENGINE_VCS3 (1u << 5)
#define I915_ENGINE_VECS0 (1u << 6)
#define I915_ENGINE_VECS1 (1u << 7)
#define I915_ENGINE_CCS0 (1u << 8)
#define I915_ENGINE_CCS1 (1u << 9)
#define I915_ENGINE_CCS2 (1u << 10)
#define I915_ENGINE_CCS3 (1u << 11)

/* Variants of a platform the workarounds and the stepping tables tell
 * apart, by device id (i915_step.c). */
enum i915_subplatform {
	I915_SUBPLATFORM_NONE = 0,
	I915_SUBPLATFORM_TGL_UY, /* Tiger Lake UP3/UP4 (the GT2 ids) */
	I915_SUBPLATFORM_ADL_N, /* Alder Lake-N */
	I915_SUBPLATFORM_RPL_S, /* Raptor Lake-S (Alder Lake-S graphics) */
	I915_SUBPLATFORM_RPL_P, /* Raptor Lake-P (Alder Lake-P graphics) */
	I915_SUBPLATFORM_RPL_U, /* Raptor Lake-U (Alder Lake-P graphics) */
	I915_SUBPLATFORM_DG2_G10,
	I915_SUBPLATFORM_DG2_G11,
	I915_SUBPLATFORM_DG2_G12,
	I915_SUBPLATFORM_BMG_G21, /* Battlemage G21 */
};

/* Silicon steppings: A0, A1, A2, A3, B0 ... J3, four to a letter, in the
 * order the hardware was revised.  0 means unknown; FUTURE is a revision
 * newer than any the tables name. */
#define I915_STEP_NONE 0
#define I915_STEP(letter, n) (1 + ((letter) - 'A') * 4 + (n))
#define I915_STEP_A0 I915_STEP('A', 0)
#define I915_STEP_A1 I915_STEP('A', 1)
#define I915_STEP_A2 I915_STEP('A', 2)
#define I915_STEP_B0 I915_STEP('B', 0)
#define I915_STEP_B1 I915_STEP('B', 1)
#define I915_STEP_C0 I915_STEP('C', 0)
#define I915_STEP_D0 I915_STEP('D', 0)
#define I915_STEP_D1 I915_STEP('D', 1)
#define I915_STEP_E0 I915_STEP('E', 0)
#define I915_STEP_F0 I915_STEP('F', 0)
#define I915_STEP_G0 I915_STEP('G', 0)
#define I915_STEP_H0 I915_STEP('H', 0)
#define I915_STEP_I1 I915_STEP('I', 1)
#define I915_STEP_J0 I915_STEP('J', 0)
#define I915_STEP_FUTURE I915_STEP('K', 0)
#define I915_STEP_FOREVER (I915_STEP_FUTURE + 1)

/* An IP version as one number: version * 100 + release (12.70 = 1270). */
#define I915_IP(ver, rel) ((ver) * 100 + (rel))

struct intel_device_info {
	const char *name; /* platform name, "Skylake" */
	uint8_t platform; /* enum i915_platform */
	uint8_t gen; /* 2..35 */
	uint16_t gen_x10; /* 80, 90, 100, 110, 120, 125, 127, 200, 201, 300, 351 */
	uint8_t display_ver; /* 9, 10, 11, 12, 13, 14, 20, 30, 35 */
	uint8_t gt; /* GT level as sold, 0 when it does not apply */
	uint8_t num_pipes; /* display pipes */
	uint8_t ppgtt_bits; /* 32 or 48 */
	uint8_t csb_entries; /* context status buffer depth: 6 or 12 */
	uint8_t dpll_model; /* enum i915_dpll_model */
	uint8_t max_dpll; /* how many shared PLLs */
	uint8_t pad;
	uint32_t flags; /* I915_INFO_* */
	uint32_t engine_mask; /* I915_ENGINE_* */
	uint32_t dma_mask_bits; /* 39 on Gen8/9, 46 from Xe_HP on */
	const char *dmc_fw; /* firmware file names, relative to /lib/firmware */
	const char *guc_fw;
	const char *huc_fw;
	const char *gsc_fw; /* the GSC's own firmware, where the part has one */
};

struct i915_pci_id {
	uint16_t id;
	uint8_t gt; /* the SKU's GT level (0 = as the platform) */
	const char *name; /* marketing name where it is worth knowing */
	const struct intel_device_info *info;
};

/* The table lookup, and a name for any device in the table. */
const struct i915_pci_id *i915_pci_lookup(uint16_t device_id);

#endif
