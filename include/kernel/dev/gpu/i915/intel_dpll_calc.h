// LikeOS -- pure PLL arithmetic shared by the driver and the host tests.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_DPLL_CALC_H
#define KERNEL_DEV_GPU_I915_INTEL_DPLL_CALC_H

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#else
#include <stdint.h>
#endif

/* A combo PLL's configuration: the DCO in integer/fractional reference
 * multiples and the three dividers as the CFGCR1 fields encode them. */
struct icl_pll_params {
	uint16_t dco_integer, dco_fraction;
	uint8_t pdiv, kdiv, qdiv_mode, qdiv_ratio;
};

int icl_combo_dp_params(uint32_t link_khz, uint32_t ref_khz,
			struct icl_pll_params *out);
int icl_combo_hdmi_params(uint32_t clock_khz, uint32_t ref_khz,
			  struct icl_pll_params *out);
uint32_t icl_combo_pll_khz(const struct icl_pll_params *p, uint32_t ref_khz);

#endif
