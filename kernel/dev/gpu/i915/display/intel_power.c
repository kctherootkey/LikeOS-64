// LikeOS -- display power wells.
//
// The display engine is split into wells that can be powered down when
// unused.  Which wells exist and which register requests them differs
// by generation: Broadwell has one well for everything beyond pipe A and
// DDI A; Skylake power well 1 (pipe A, the eDP transcoder, AUX A), power
// well 2 (the other pipes, ports B-D) and a well per DDI; Broxton PW1/PW2
// and two DPIO PHYs; Ice Lake and Tiger Lake a chain of pipe wells and a
// well per DDI and per AUX channel in registers of their own.  A domain
// names what a caller is about to touch; get/put keep the wells it
// needs up, refcounted.  A request goes through the driver's control
// register; the state bit answers when the well is up.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* A well: the register its request and state bits are in, and their
 * index there. */
struct well {
	uint32_t reg;
	int idx;
};

static int well_wait(struct i915_device *i915, struct well w, int up)
{
	for (int t = 0; t < 1000; t++) {
		uint32_t v = i915_read32(i915, w.reg);
		if (!!(v & HSW_PWR_WELL_CTL_STATE(w.idx)) == up)
			return 0;
		lapic_delay_us(10);
	}
	return -ETIMEDOUT;
}

static int well_enable(struct i915_device *i915, struct well w)
{
	uint32_t v = i915_read32(i915, w.reg);
	if (v & HSW_PWR_WELL_CTL_STATE(w.idx))
		return 0;
	i915_write32(i915, w.reg, v | HSW_PWR_WELL_CTL_REQ(w.idx));
	if (well_wait(i915, w, 1) == 0)
		return 0;
	kprintf("[drm] i915: power well %d (register %05x) did not come up\n", w.idx,
		w.reg);
	return -ETIMEDOUT;
}

static void well_disable(struct i915_device *i915, struct well w)
{
	uint32_t v = i915_read32(i915, w.reg);
	if (!(v & HSW_PWR_WELL_CTL_REQ(w.idx)))
		return;
	i915_write32(i915, w.reg, v & ~HSW_PWR_WELL_CTL_REQ(w.idx));
}

static struct well W(uint32_t reg, int idx)
{
	struct well w = { reg, idx };
	return w;
}

/* The wells a domain needs, in the order they come up (each on the one
 * before it); returns the count (0: always-on). */
static int domain_wells(struct i915_device *i915, enum intel_power_domain d,
			struct well *out, int cap)
{
	enum intel_display_model model = i915->display.model;
	int n = 0;
	int port = -1;
	int aux = -1;
	int pipe = -1;

	if (d >= INTEL_PW_DDI_A && d <= INTEL_PW_DDI_I)
		port = d - INTEL_PW_DDI_A;
	else if (d >= INTEL_PW_AUX_A && d <= INTEL_PW_AUX_I)
		aux = d - INTEL_PW_AUX_A;
	else if (d >= INTEL_PW_PIPE_A && d <= INTEL_PW_PIPE_D)
		pipe = d - INTEL_PW_PIPE_A;
	(void)cap;

	switch (model) {
	case INTEL_DISPLAY_BDW:
		/* pipe A, its transcoder, DDI A and AUX A are always on */
		if (pipe == 0 || d == INTEL_PW_TRANSCODER_EDP || port == 0 || aux == 0 ||
		    d == INTEL_PW_DISPLAY_CORE)
			return 0;
		out[n++] = W(HSW_PWR_WELL_CTL2, HSW_PW_CTL_IDX_GLOBAL);
		return n;
	case INTEL_DISPLAY_SKL:
		if (d == INTEL_PW_DISPLAY_CORE) {
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_1);
			return n;
		}
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_1);
		if (pipe == 0 || d == INTEL_PW_TRANSCODER_EDP || aux == 0 || port == 0 ||
		    port == 4) {
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_A_E);
			return n;
		}
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_2);
		if (port == 1)
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_B);
		else if (port == 2)
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_C);
		else if (port == 3)
			out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_DDI_D);
		return n;
	case INTEL_DISPLAY_BXT:
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_1);
		if (d == INTEL_PW_DISPLAY_CORE || pipe == 0 || d == INTEL_PW_TRANSCODER_EDP ||
		    aux == 0 || port == 0)
			return n;
		out[n++] = W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_PW_2);
		return n;
	case INTEL_DISPLAY_ICL:
	case INTEL_DISPLAY_TGL:
		out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_1);
		if (d == INTEL_PW_DISPLAY_CORE)
			return n;
		if (pipe == 0 || d == INTEL_PW_TRANSCODER_EDP) {
			return n;
		}
		if (port == 0) {
			out[n++] = W(ICL_PWR_WELL_CTL_DDI2, ICL_PW_CTL_IDX_DDI(0));
			return n;
		}
		if (aux == 0) {
			out[n++] = W(ICL_PWR_WELL_CTL_AUX2, ICL_PW_CTL_IDX_AUX(0));
			return n;
		}
		/* everything else sits behind the pipe-well chain */
		out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_2);
		out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_3);
		if (model == INTEL_DISPLAY_TGL) {
			if (pipe == 2 || pipe == 3)
				out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_4);
			if (pipe == 3)
				out[n++] = W(HSW_PWR_WELL_CTL2, TGL_PW_CTL_IDX_PW_5);
		} else if (pipe == 2) {
			out[n++] = W(HSW_PWR_WELL_CTL2, ICL_PW_CTL_IDX_PW_4);
		}
		if (port > 0)
			out[n++] = W(ICL_PWR_WELL_CTL_DDI2, ICL_PW_CTL_IDX_DDI(port));
		if (aux > 0)
			out[n++] = W(ICL_PWR_WELL_CTL_AUX2, ICL_PW_CTL_IDX_AUX(aux));
		return n;
	}
	return 0;
}

static int well_needed(struct i915_device *i915, struct well w)
{
	struct well ws[8];
	for (int d = 0; d < INTEL_PW_COUNT; d++) {
		if (!i915->display.pw_count[d])
			continue;
		int n = domain_wells(i915, (enum intel_power_domain)d, ws, 8);
		for (int i = 0; i < n; i++)
			if (ws[i].reg == w.reg && ws[i].idx == w.idx)
				return 1;
	}
	return 0;
}

void intel_power_get(struct i915_device *i915, enum intel_power_domain d)
{
	struct well ws[8];
	int n = domain_wells(i915, d, ws, 8);

	i915->display.pw_count[d]++;
	for (int i = 0; i < n; i++)
		well_enable(i915, ws[i]);
}

void intel_power_put(struct i915_device *i915, enum intel_power_domain d)
{
	struct well ws[8];
	int n = domain_wells(i915, d, ws, 8);

	if (i915->display.pw_count[d] > 0)
		i915->display.pw_count[d]--;
	/* PW1 (and Broadwell's one well) stay up while the driver runs. */
	for (int i = n - 1; i >= 1; i--)
		if (!well_needed(i915, ws[i]))
			well_disable(i915, ws[i]);
}

/* Display core bring-up, in the order the hardware wants: PW1 (and the
 * misc I/O well on Skylake), then the CDCLK (already running from the
 * firmware; see intel_cdclk.c), then the data buffer.  PW1 is never
 * released. */
int intel_power_init(struct i915_device *i915)
{
	enum intel_display_model model = i915->display.model;

	for (int d = 0; d < INTEL_PW_COUNT; d++)
		i915->display.pw_count[d] = 0;
	if (model == INTEL_DISPLAY_BDW) {
		/* The one well is left as the firmware had it: PW1-style
		 * bring-up does not exist here. */
		i915->display.pw_count[INTEL_PW_DISPLAY_CORE] = 1;
		return 0;
	}
	/* DC states off while the driver runs (until DMC is loaded and
	 * wanted): a display that goes to DC5/DC6 under a driver that did
	 * not ask for it loses register state. */
	uint32_t dc = i915_read32(i915, DC_STATE_EN);
	if (dc & DC_STATE_EN_UPTO_DC5_DC6_MASK)
		i915_write32(i915, DC_STATE_EN, dc & ~DC_STATE_EN_UPTO_DC5_DC6_MASK);

	int pw1 = (model == INTEL_DISPLAY_ICL || model == INTEL_DISPLAY_TGL) ?
			  ICL_PW_CTL_IDX_PW_1 :
			  SKL_PW_CTL_IDX_PW_1;
	if (well_enable(i915, W(HSW_PWR_WELL_CTL2, pw1)) != 0)
		return -EIO;
	/* the fuse distribution for PG1 must have completed */
	for (int t = 0; t < 100; t++) {
		if (i915_read32(i915, SKL_FUSE_STATUS) & SKL_FUSE_PG_DIST_STATUS(1))
			break;
		lapic_delay_us(10);
	}
	if (model == INTEL_DISPLAY_SKL)
		well_enable(i915, W(HSW_PWR_WELL_CTL2, SKL_PW_CTL_IDX_MISC_IO));
	if (model == INTEL_DISPLAY_ICL || model == INTEL_DISPLAY_TGL) {
		/* the data buffer slices, which the planes read through */
		uint32_t regs[2] = { DBUF_CTL_S1, DBUF_CTL_S2 };
		for (int s = 0; s < 2; s++) {
			uint32_t v = i915_read32(i915, regs[s]);
			if (v & DBUF_POWER_STATE)
				continue;
			i915_write32(i915, regs[s], v | DBUF_POWER_REQUEST);
			for (int t = 0; t < 100; t++) {
				if (i915_read32(i915, regs[s]) & DBUF_POWER_STATE)
					break;
				lapic_delay_us(10);
			}
		}
	}
	i915->display.pw_count[INTEL_PW_DISPLAY_CORE] = 1;
	return 0;
}

void intel_power_fini(struct i915_device *i915)
{
	/* Leave every well the firmware had on: we may be handing the
	 * screen back to its framebuffer. */
	(void)i915;
}
