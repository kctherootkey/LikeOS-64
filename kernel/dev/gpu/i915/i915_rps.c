// LikeOS -- the GT's clock: render P-states.
//
// Left to the firmware the GT idles at its lowest P-state and stays
// there: nothing in this driver runs the up/down evaluation the hardware
// offers, so a renderer would work at a third of the part's speed.  The
// policy here is the plain one the plan names -- ask for the highest
// non-turbo frequency once and leave the hardware's own controller off,
// so the request is what runs (the part still throttles itself for heat
// and power).  Nothing to do on a platform whose GuC owns the clocks.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* A P-state value in MHz for the log. */
static uint32_t rps_mhz(const struct i915_device *i915, uint32_t val)
{
	if (i915->info->gen >= 9)
		return val * 50 / 3;
	return val * 50;
}

int i915_rps_init(struct i915_device *i915)
{
	if (i915->info->gen < 8 || i915->info->gen > 12)
		return 0;
	if (i915->info->flags & I915_INFO_GUC_MANDATORY)
		return 0; /* the GuC's SLPC runs the clocks there */
	uint32_t cap = i915_read32(i915, GEN6_RP_STATE_CAP);
	uint32_t rp0 = cap & 0xff;
	uint32_t rp1 = (cap >> 8) & 0xff;
	uint32_t rpn = (cap >> 16) & 0xff;
	if (!rp0 || !rpn || rp0 < rpn) {
		kprintf("[drm] i915: no P-state capabilities (%08x); the clock stays as found\n",
			cap);
		return -ENODEV;
	}
	/* RP1 is the guaranteed frequency; RP0 the highest.  Ask for RP0:
	 * the hardware caps it itself when it must. */
	uint32_t want = rp0;
	uint32_t req = i915->info->gen >= 9 ? GEN9_FREQUENCY(want) : GEN6_FREQUENCY(want);
	i915_fw_get(i915, I915_FW_RENDER);
	i915_write32(i915, GEN6_RP_CONTROL, 0); /* no evaluation: the request stands */
	i915_write32(i915, GEN6_RP_INTERRUPT_LIMITS, 0);
	i915_write32(i915, GEN6_RPNSWREQ, req);
	(void)i915_read32(i915, GEN6_RPNSWREQ);
	i915_fw_put(i915, I915_FW_RENDER);
	i915->rps_rp0 = rp0;
	i915->rps_rp1 = rp1;
	i915->rps_rpn = rpn;
	i915->rps_cur = want;
	kprintf("[drm] i915: GT clock requested at %u MHz (RPn %u, RP1 %u, RP0 %u MHz)\n",
		rps_mhz(i915, want), rps_mhz(i915, rpn), rps_mhz(i915, rp1),
		rps_mhz(i915, rp0));
	return 0;
}

/* What the GT is running at right now, in MHz (0 when unknown). */
uint32_t i915_rps_current_mhz(struct i915_device *i915)
{
	if (i915->info->gen < 9 || i915->info->gen > 12)
		return 0;
	uint32_t st = i915_read32(i915, GEN6_RPSTAT1);
	return rps_mhz(i915, (st & GEN9_CAGF_MASK) >> GEN9_CAGF_SHIFT);
}
