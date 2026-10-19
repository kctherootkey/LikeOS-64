// LikeOS -- pure PLL arithmetic shared by the driver and the host tests.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2023 Intel Corporation

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

/* The DisplayPort link rates from the hardware's table; 38.4 MHz uses the
 * 19.2 MHz entries (the PLL halves that reference itself). */
int icl_combo_dp_params(uint32_t link_khz, uint32_t ref_khz,
			struct icl_pll_params *out);
/* HDMI (and DSI): the divider that puts the DCO nearest the middle of
 * 7998..10000 MHz. */
int icl_combo_hdmi_params(uint32_t clock_khz, uint32_t ref_khz,
			  struct icl_pll_params *out);
/* The port clock a set of parameters gives (for checks and readout). */
uint32_t icl_combo_pll_khz(const struct icl_pll_params *p, uint32_t ref_khz);

/* Skylake's WRPLL in HDMI mode: DCO integer/fraction of its reference clock,
 * the three dividers in their CFGCR2 encoding, and which of the three
 * central frequencies the DCO sits near (also in its CFGCR2 encoding:
 * 0 = 9600, 1 = 9000, 3 = 8400 MHz). */
struct skl_wrpll_params {
	uint32_t dco_fraction, dco_integer;
	uint32_t qdiv_ratio, qdiv_mode, kdiv, pdiv, central_freq;
};

int skl_wrpll_calc(uint32_t clock_khz, uint32_t ref_khz, struct skl_wrpll_params *out);
/* The port clock a CFGCR1/CFGCR2 pair gives, in kHz (0 if invalid). */
uint32_t skl_wrpll_khz(uint32_t cfgcr1, uint32_t cfgcr2, uint32_t ref_khz);

/* Haswell/Broadwell's WRPLL from the 2.7 GHz LCPLL: reference divider
 * r2, feedback n2 and post divider p as the register holds them. */
void hsw_wrpll_calc(uint32_t clock_khz, uint32_t *r2, uint32_t *n2, uint32_t *p);
/* The port clock those give, in kHz (the LCPLL as reference). */
uint32_t hsw_wrpll_khz(uint32_t r2, uint32_t n2, uint32_t p);

/* Broxton/Gemini Lake's port PLL: n = 1, m1 = 2, m2 in 22.22 fixed
 * point, and the two post dividers. */
struct bxt_pll_dividers {
	uint32_t p1, p2, n, m1, m2;
	uint32_t vco, dot; /* kHz */
};

/* The DisplayPort table entry for a link rate (-1: not a rate the PLL
 * has an entry for). */
int bxt_dp_dividers(uint32_t link_khz, struct bxt_pll_dividers *out);
/* The best dividers for an HDMI clock from the 100 MHz reference. */
int bxt_hdmi_dividers(uint32_t clock_khz, struct bxt_pll_dividers *out);
/* vco and dot from p1, p2, n, m1, m2 (refclk 100 MHz); returns dot. */
uint32_t bxt_pll_calc_params(struct bxt_pll_dividers *d);

#endif
