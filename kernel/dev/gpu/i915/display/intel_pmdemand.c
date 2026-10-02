// LikeOS -- PM demand: telling the Punit what the display needs.
//
// From display version 14 the display no longer raises its voltage and
// memory bandwidth through pcode mailbox calls around each change.
// Instead it keeps a standing request in two registers -- how many pipes,
// DBUF slices, non-Type-C PHYs and PLLs are active, the CDCLK and the
// fastest port clock, the voltage level, the memory bandwidth the planes
// may draw -- and sets the request bit; the Punit answers by clearing
// it once it has adjusted.  A requirement must be in place before the
// hardware that has it starts, and is only lowered once that hardware
// has stopped: before a port comes up the request is raised to the
// larger of what is running and what is about to run; after a change has
// settled it is set to what is running.
//
// The memory bandwidth field carries the peak bandwidth of the memory
// frequency point the display needs held.  Unless the bandwidth code
// has chosen one (intel_pmdemand_set_qgv_peakbw()), it asks for all
// ones -- "keep the memory where it is" -- as a display whose
// watermarks may not ride out a frequency change must.
//
// Display version 20 (Lunar Lake) keeps the request as it is; version 30
// (Panther Lake on) widens the pipe count to the four pipes and drops
// the DBUF slice and scaler counts from it, and takes no voltage level
// (the power controller works that out from the CDCLK itself).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2023-2025 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_xelpdp_regs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

struct pmdemand_params {
	uint32_t qclk_gv_bw;
	uint32_t voltage_index;
	uint32_t qclk_gv_index;
	uint32_t active_pipes;
	uint32_t active_dbufs;
	uint32_t active_phys;
	uint32_t plls;
	uint32_t cdclk_freq_mhz;
	uint32_t ddiclk_max;
	uint32_t scalers;
};

static int has_pmdemand(struct i915_device *i915)
{
	return i915->info->display_ver >= 14 && i915->display.model == INTEL_DISPLAY_MTL;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

static uint32_t popcount(uint32_t v)
{
	uint32_t n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}

static int wait_clear(struct i915_device *i915, uint32_t reg, uint32_t bit, uint32_t ms)
{
	for (uint32_t t = 0; t < ms * 100; t++) {
		if (!(i915_read32(i915, reg) & bit))
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

/* A request may only go out once the previous one was answered and
 * nothing is in flight. */
static int prev_transaction_done(struct i915_device *i915)
{
	return wait_clear(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1), XELPDP_PMDEMAND_REQ_ENABLE,
			  10) == 0 &&
	       wait_clear(i915, GEN12_DCPR_STATUS_1, XELPDP_PMDEMAND_INFLIGHT_STATUS, 10) == 0;
}

/* Send what is in the registers and wait for the Punit to take it
 * (the response interrupt is not used: the request bit clears). */
static void pmdemand_kick(struct i915_device *i915)
{
	uint32_t r1 = i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1));
	i915_write32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1), r1 | XELPDP_PMDEMAND_REQ_ENABLE);
	if (wait_clear(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1), XELPDP_PMDEMAND_REQ_ENABLE,
		       10))
		kprintf("[drm] i915: the Punit did not answer the PM demand request (%08x %08x)\n",
			i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(0)),
			i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1)));
}

int intel_pmdemand_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;

	if (!has_pmdemand(i915))
		return 0;
	/* Meteor Lake's early steppings (A0 up to C0) time the response
	 * out on their own; that check is turned off. */
	if (intel_display_verx100(i915) == 1400 && d->display_step != 0xff &&
	    d->display_step < 8) {
		uint32_t v = i915_read32(i915, XELPD_CHICKEN_DCPR_3);
		i915_write32(i915, XELPD_CHICKEN_DCPR_3, v | DMD_RSP_TIMEOUT_DISABLE);
	}
	for (int p = 0; p < INTEL_MAX_PIPES; p++)
		d->pmdemand_ddiclk_khz[p] = 0;
	if (!prev_transaction_done(i915)) {
		kprintf("[drm] i915: a PM demand request is still pending from the firmware\n");
		d->pmdemand_ready = 1;
		return -EBUSY;
	}
	uint32_t r0 = i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(0));
	uint32_t r1 = i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1));
	i915_dbg("[drm] i915: PM demand from the firmware: %u pipes, %u DBUF, %u PHYs, %u PLLs, CDCLK %u MHz, port clock %u MHz, voltage %u, bandwidth %u\n",
		 i915->info->display_ver >= 30 ?
			 (r0 & XE3_PMDEMAND_PIPES_MASK) >> XE3_PMDEMAND_PIPES_SHIFT :
			 (r0 & XELPDP_PMDEMAND_PIPES_MASK) >> XELPDP_PMDEMAND_PIPES_SHIFT,
		 (r0 & XELPDP_PMDEMAND_DBUFS_MASK) >> XELPDP_PMDEMAND_DBUFS_SHIFT,
		 (r0 & XELPDP_PMDEMAND_PHYS_MASK) >> XELPDP_PMDEMAND_PHYS_SHIFT,
		 (r1 & XELPDP_PMDEMAND_PLLS_MASK) >> XELPDP_PMDEMAND_PLLS_SHIFT,
		 (r1 & XELPDP_PMDEMAND_CDCLK_FREQ_MASK) >> XELPDP_PMDEMAND_CDCLK_FREQ_SHIFT,
		 (r1 & XELPDP_PMDEMAND_DDICLK_FREQ_MASK) >> XELPDP_PMDEMAND_DDICLK_FREQ_SHIFT,
		 (r0 & XELPDP_PMDEMAND_VOLTAGE_INDEX_MASK) >> XELPDP_PMDEMAND_VOLTAGE_INDEX_SHIFT,
		 (r0 & XELPDP_PMDEMAND_QCLK_GV_BW_MASK) >> XELPDP_PMDEMAND_QCLK_GV_BW_SHIFT);
	d->pmdemand_ready = 1;
	return 0;
}

/* Part of the display init sequence: the DBUF slice count goes to the
 * Punit before the slices are powered (and after they are turned off).
 * Version 30 does not track the slices. */
void intel_pmdemand_program_dbuf(struct i915_device *i915, uint8_t slices)
{
	if (!has_pmdemand(i915) || i915->info->display_ver >= 30)
		return;
	if (!prev_transaction_done(i915)) {
		kprintf("[drm] i915: PM demand busy; DBUF slice count not sent\n");
		return;
	}
	uint32_t r0 = i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(0));
	r0 &= ~XELPDP_PMDEMAND_DBUFS_MASK;
	r0 |= min_u32(popcount(slices), 3) << XELPDP_PMDEMAND_DBUFS_SHIFT;
	i915_write32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(0), r0);
	pmdemand_kick(i915);
}

/* The voltage level the CDCLK needs on this display (the same steps as
 * Raptor Lake U): 312, 480, 556.8 and 652.8 MHz. */
static uint32_t cdclk_voltage_level(uint32_t cdclk_khz)
{
	static const uint32_t max_cdclk[] = { 312000, 480000, 556800, 652800 };
	for (uint32_t i = 0; i < 4; i++)
		if (cdclk_khz <= max_cdclk[i])
			return i;
	return 3;
}

/* The clock an active output's port runs at: the link rate of a
 * DisplayPort link, the TMDS character rate of an HDMI one. */
static uint32_t output_port_clock(struct i915_device *i915, const struct intel_output *o)
{
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP)
		return o->link_rate_khz;
	if (o->pipe >= 0 && o->pipe < INTEL_MAX_PIPES)
		return i915->display.pipes[o->pipe].pixel_rate_khz;
	return 0;
}

/* What the display needs with the outputs active now, plus `extra' on
 * `extra_pipe' at `extra_clock' when given. */
static void compute(struct i915_device *i915, const struct intel_output *extra, int extra_pipe,
		    uint32_t extra_clock, struct pmdemand_params *p)
{
	struct intel_display *d = &i915->display;
	uint32_t pipes = 0, phys = 0, ddiclk = 0;
	uint32_t clocks[INTEL_MAX_PIPES] = { 0 };

	for (int i = 0; i < INTEL_MAX_PIPES && i < i915->info->num_pipes; i++)
		if (d->pipes[i].active)
			pipes |= 1u << i;
	for (int i = 0; i < d->nout; i++) {
		const struct intel_output *o = &d->outputs[i];
		if (!o->active)
			continue;
		/* The Type-C PHYs are the Type-C subsystem's to power; only
		 * the others count. */
		if (!o->is_tc)
			phys |= 1u << o->port;
		if (o->pipe >= 0 && o->pipe < INTEL_MAX_PIPES)
			clocks[o->pipe] = output_port_clock(i915, o);
	}
	if (extra && extra_pipe >= 0 && extra_pipe < INTEL_MAX_PIPES) {
		pipes |= 1u << extra_pipe;
		if (!extra->is_tc)
			phys |= 1u << extra->port;
		clocks[extra_pipe] = extra_clock;
	}
	for (int i = 0; i < INTEL_MAX_PIPES; i++) {
		d->pmdemand_ddiclk_khz[i] = clocks[i];
		ddiclk = max_u32(ddiclk, clocks[i]);
	}
	p->qclk_gv_bw = d->pmdemand_qgv_peakbw ? d->pmdemand_qgv_peakbw : 0xffff;
	p->qclk_gv_index = 0; /* the firmware picks the point */
	/* version 30 counts up to the pipes the part has, before it at
	 * most 3 ("three or more") */
	if (i915->info->display_ver >= 30)
		p->active_pipes = min_u32(popcount(pipes), i915->info->num_pipes);
	else
		p->active_pipes = min_u32(popcount(pipes), 3);
	p->active_dbufs = min_u32(popcount(d->dbuf_slices), 3);
	p->active_phys = min_u32(popcount(phys), 7);
	/* the CDCLK PLL is one of them */
	p->plls = min_u32(p->active_phys + 1, 7);
	p->cdclk_freq_mhz = (d->cdclk_khz + 999) / 1000;
	if (i915->info->display_ver >= 30)
		p->voltage_index = d->cdclk_voltage_level; /* 0: not used */
	else
		p->voltage_index = max_u32(cdclk_voltage_level(d->cdclk_khz),
					   d->cdclk_voltage_level);
	p->ddiclk_max = (ddiclk + 999) / 1000;
	/* cannot be worked out per frame without the scaler state: all */
	p->scalers = 7;
}

#define FIELD_SET(reg, val, mask, shift, raise)                                         \
	do {                                                                           \
		uint32_t cur_ = ((reg) & (mask)) >> (shift);                          \
		uint32_t new_ = (raise) ? max_u32(cur_, (val)) : (val);               \
		(reg) = ((reg) & ~(mask)) | ((new_ << (shift)) & (mask));            \
	} while (0)

/* Program the request: with `raise', each field to the larger of what
 * it holds and what is needed; else to exactly what is needed.  The
 * request bit is only set when something changed. */
static void program(struct i915_device *i915, const struct pmdemand_params *p, int raise)
{
	if (!prev_transaction_done(i915)) {
		kprintf("[drm] i915: PM demand busy; request not updated\n");
		return;
	}
	uint32_t r0 = i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(0));
	uint32_t r1 = i915_read32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1));
	uint32_t n0 = r0, n1 = r1;

	FIELD_SET(n0, p->qclk_gv_bw, XELPDP_PMDEMAND_QCLK_GV_BW_MASK,
		  XELPDP_PMDEMAND_QCLK_GV_BW_SHIFT, raise);
	FIELD_SET(n0, p->voltage_index, XELPDP_PMDEMAND_VOLTAGE_INDEX_MASK,
		  XELPDP_PMDEMAND_VOLTAGE_INDEX_SHIFT, raise);
	FIELD_SET(n0, p->qclk_gv_index, XELPDP_PMDEMAND_QCLK_GV_INDEX_MASK,
		  XELPDP_PMDEMAND_QCLK_GV_INDEX_SHIFT, raise);
	FIELD_SET(n0, p->active_phys, XELPDP_PMDEMAND_PHYS_MASK, XELPDP_PMDEMAND_PHYS_SHIFT,
		  raise);
	if (i915->info->display_ver >= 30) {
		FIELD_SET(n0, p->active_pipes, XE3_PMDEMAND_PIPES_MASK, XE3_PMDEMAND_PIPES_SHIFT,
			  raise);
	} else {
		FIELD_SET(n0, p->active_pipes, XELPDP_PMDEMAND_PIPES_MASK,
			  XELPDP_PMDEMAND_PIPES_SHIFT, raise);
		FIELD_SET(n0, p->active_dbufs, XELPDP_PMDEMAND_DBUFS_MASK,
			  XELPDP_PMDEMAND_DBUFS_SHIFT, raise);
	}
	FIELD_SET(n1, p->cdclk_freq_mhz, XELPDP_PMDEMAND_CDCLK_FREQ_MASK,
		  XELPDP_PMDEMAND_CDCLK_FREQ_SHIFT, raise);
	FIELD_SET(n1, p->ddiclk_max, XELPDP_PMDEMAND_DDICLK_FREQ_MASK,
		  XELPDP_PMDEMAND_DDICLK_FREQ_SHIFT, raise);
	FIELD_SET(n1, p->plls, XELPDP_PMDEMAND_PLLS_MASK, XELPDP_PMDEMAND_PLLS_SHIFT, raise);
	if (i915->info->display_ver < 30)
		FIELD_SET(n1, p->scalers, XELPDP_PMDEMAND_SCALERS_MASK,
			  XELPDP_PMDEMAND_SCALERS_SHIFT, raise);
	if (n0 == r0 && n1 == r1)
		return;
	i915_write32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(0), n0);
	i915_write32(i915, XELPDP_INITIATE_PMDEMAND_REQUEST(1), n1);
	i915_dbg("[drm] i915: PM demand request %08x %08x\n", n0, n1);
	pmdemand_kick(i915);
}

void intel_pmdemand_set_qgv_peakbw(struct i915_device *i915, uint16_t peakbw)
{
	i915->display.pmdemand_qgv_peakbw = peakbw;
}

void intel_pmdemand_pre_enable(struct i915_device *i915, const struct intel_output *o,
			       int pipe, uint32_t port_clock_khz)
{
	struct pmdemand_params p;

	if (!has_pmdemand(i915) || !i915->display.pmdemand_ready)
		return;
	compute(i915, o, pipe, port_clock_khz, &p);
	program(i915, &p, 1);
}

void intel_pmdemand_update(struct i915_device *i915)
{
	struct pmdemand_params p;

	if (!has_pmdemand(i915) || !i915->display.pmdemand_ready)
		return;
	compute(i915, NULL, -1, 0, &p);
	/* While the firmware's pipe still scans out, what it needs (its pipe,
	 * its PHY, its port clock) is in none of the driver's state: nothing
	 * is lowered under it, only raised. */
	program(i915, &p, i915->boot_scanout.pipe >= 0);
}
