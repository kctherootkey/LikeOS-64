// LikeOS -- the Synopsys port PHYs of DG2: what the display code calls.
//
// The counterpart of the Meteor Lake PHY interface in intel_display.h
// (mtl_phy_*) for display version 13's discrete part.  Every PHY has its
// own PLL, so a port's PLL id is the port itself; enabling that PLL is
// what clocks the port (there is no separate routing step).  The
// sequences take the output because its type (DP, eDP or HDMI), its
// lane count and its reversal decide what is written.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_I915_INTEL_SNPS_PHY_H
#define KERNEL_DEV_GPU_I915_INTEL_SNPS_PHY_H

#include <kernel/uapi/types.h>

struct i915_device;
struct intel_output;

/* The PHY behind a DDI (SNPS_PHY_A..E), or -1 when the port has none. */
int dg2_port_to_phy(int port);

/* The DisplayPort 8b/10b rates the PHY has PLL settings for (the eDP
 * rates included); the 128b/132b rates are left out, the link code not
 * driving them. */
int dg2_phy_rate_supported(struct i915_device *i915, int port, uint32_t link_rate_khz);
/* The highest link rate the port's output may train at: 8.1 Gbps for an
 * embedded panel, 13.5 Gbps for an external DisplayPort sink. */
uint32_t dg2_phy_max_link_rate(struct i915_device *i915, const struct intel_output *o);

/* PLL settings for a DP link rate or a TMDS clock (kHz), kept for the
 * port; returns the PLL id (the port) or a negative errno.  For HDMI a
 * clock with no table entry gets settings computed from the PHY's
 * characterisation curves.  `ssc' is recorded; the tables decide
 * (the embedded-panel rates and the 128b/132b rates spread, the others
 * do not). */
int dg2_phy_pll_get_dp(struct i915_device *i915, struct intel_output *o,
		       uint32_t link_rate_khz, int ssc);
int dg2_phy_pll_get_hdmi(struct i915_device *i915, struct intel_output *o, uint32_t clock_khz);
void dg2_phy_pll_put(struct i915_device *i915, int pll);

/* The PLL programmed, enabled, forced on and locked (the port then has
 * its clock); `lanes' is the link width (4 for HDMI), recorded for the
 * signal levels.  Disable: the reverse. */
int dg2_phy_pll_enable(struct i915_device *i915, struct intel_output *o, int lanes);
void dg2_phy_pll_disable(struct i915_device *i915, struct intel_output *o);

/* The port clock now running, in kHz (DP: the link rate; HDMI: the TMDS
 * clock), read back from the PHY's PLL; 0 when it is off. */
uint32_t dg2_phy_port_link_rate(struct i915_device *i915, struct intel_output *o);

/* Voltage swing and pre-emphasis into the PHY's transmitter
 * equalisation, all four lanes: `level' is intel_ddi_dp_level_for()'s
 * index for DP, the HDMI table's for TMDS (-1: the default entry).  On a
 * 128b/132b link it is a transmitter preset (0..15) instead. */
void dg2_phy_set_signal_level(struct i915_device *i915, struct intel_output *o, int level);

/* The DDI buffer of a port on a Synopsys PHY: width, reversal and the
 * translation select (always 0 -- the levels are the PHY's), then enable
 * and wait for it to leave idle; and back off. */
void dg2_ddi_buf_enable(struct i915_device *i915, struct intel_output *o, int lanes);
void dg2_ddi_buf_disable(struct i915_device *i915, struct intel_output *o);

/* The lanes' power state while the panel self-refreshes. */
void dg2_phy_psr_power_state(struct i915_device *i915, struct intel_output *o, int enable);

/* Once at display init (after the display core, CDCLK and the data
 * buffer are up): wait for every PHY to finish its calibration and
 * adaptation, note any that did not, and say what the firmware left in
 * the PLLs.  Also clears the driver's PLL bookkeeping. */
int dg2_phy_init(struct i915_device *i915);
/* A PHY that did not finish calibrating; its port is driven anyway. */
int dg2_phy_failed_calibration(int port);

#endif
