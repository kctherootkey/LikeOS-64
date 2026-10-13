// LikeOS -- the GT's clock: render P-states.
//
// Left to the firmware the GT idles at its lowest P-state and stays
// there, so a renderer works at a third of the part's speed or less.  The
// policy here is the plain one the plan names: ask for the highest
// non-turbo frequency once.  The P-unit takes a software request only
// with its evaluation running -- with GEN6_RP_CONTROL clear the request
// is ignored and the GT stays where the firmware left it (it read back
// 100 MHz) -- so the evaluation is programmed and enabled the way the
// interrupt-driven policy programs it, with its interrupts masked: the
// request stands, and the part still throttles itself for heat and
// power.  Every Gen8-12 part fed by execlists goes through here; the
// parts whose GuC owns the clocks are set through SLPC (i915_guc.c),
// Gen6/7 and the Atom parts by i915_legacy_rps.c.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/i915_legacy.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* A P-state value in MHz for the log. */
static uint32_t rps_mhz(const struct i915_device *i915, uint32_t val)
{
	if (i915->info->gen >= 9)
		return val * 50 / 3;
	return val * 50;
}

static int rps_is_gen9_lp(const struct i915_device *i915)
{
	return i915->info->platform == I915_PLATFORM_BROXTON ||
	       i915->info->platform == I915_PLATFORM_GEMINILAKE;
}

/* The request register's encoding of a P-state value. */
static uint32_t rps_freq_field(const struct i915_device *i915, uint32_t val)
{
	return i915->info->gen >= 9 ? GEN9_FREQUENCY(val) : BDW_FREQUENCY(val);
}

/* Nanoseconds in the evaluation's unit: 16 ticks of the command
 * streamer's timestamp clock. */
static uint32_t rps_pm_interval(const struct i915_device *i915, uint64_t ns)
{
	uint64_t hz = i915->cs_timestamp_hz ? i915->cs_timestamp_hz : 12000000ULL;
	uint64_t clocks = (ns * hz + 999999999ULL) / 1000000000ULL;
	return (uint32_t)((clocks + 15) / 16);
}

int i915_rps_init(struct i915_device *i915)
{
	if (i915->info->gen < 8 || i915->info->gen > 12)
		return 0;
	if (i915->info->flags & I915_INFO_GUC_MANDATORY)
		return 0; /* the GuC's SLPC runs the clocks there */
	/* Cherryview's requests go to the P-unit over the sideband, not
	 * to these registers: its clock is the legacy code's */
	if (i915->info->platform == I915_PLATFORM_CHERRYVIEW)
		return 0;
	/* The capabilities come in the request's own unit everywhere but
	 * on the big-core Gen9 parts and Gen10 onwards: those report 50 MHz
	 * steps while the request counts 50/3 MHz steps, so the values are
	 * scaled by three before use.  Broxton and Gemini Lake keep their
	 * capabilities in a register of their own, RP0 at the top. */
	uint32_t cap, rp0, rp1, rpn;
	if (rps_is_gen9_lp(i915)) {
		cap = i915_read32(i915, BXT_RP_STATE_CAP);
		rp0 = (cap >> 16) & 0xff;
		rp1 = (cap >> 8) & 0xff;
		rpn = cap & 0xff;
	} else {
		cap = i915_read32(i915, GEN6_RP_STATE_CAP);
		rp0 = cap & 0xff;
		rp1 = (cap >> 8) & 0xff;
		rpn = (cap >> 16) & 0xff;
	}
	if (!rp0 || !rpn || rp0 < rpn) {
		kprintf("[drm] i915: no P-state capabilities (%08x); the clock stays as found\n",
			cap);
		return -ENODEV;
	}
	if (i915->info->gen >= 10 || (i915->info->gen == 9 && !rps_is_gen9_lp(i915))) {
		rp0 *= GEN9_FREQ_SCALER;
		rp1 *= GEN9_FREQ_SCALER;
		rpn *= GEN9_FREQ_SCALER;
	}
	if (rp1 < rpn || rp1 > rp0)
		rp1 = rpn;
	/* RP1 is the guaranteed frequency; RP0 the highest.  Ask for RP0:
	 * the hardware caps it itself when it must. */
	uint32_t want = rp0;
	/* the evaluation interval and thresholds of the busiest setting */
	const uint32_t ei_up = 10000, up_pct = 85, ei_down = 32000, down_pct = 60;
	/* the interrupt limits: never above RP0; the floor only when the
	 * request is at it */
	uint32_t limits = i915->info->gen >= 9 ? (rp0 << 23) : (rp0 << 24);
	if (want <= rpn)
		limits |= i915->info->gen >= 9 ? (rpn << 14) : (rpn << 16);
	/* every evaluation interrupt masked: nothing here acts on them.
	 * Gen8-10 keep the redirection-to-GuC bit clear. */
	uint32_t pm_mask = ~0u;
	if (i915->info->gen < 11)
		pm_mask &= ~GEN8_PMINTR_DISABLE_REDIRECT_TO_GUC;

	i915_fw_get(i915, I915_FW_ALL);
	if (i915->info->gen == 8)
		i915_write32(i915, GEN6_RC_VIDEO_FREQ, BDW_FREQUENCY(rp1));
	else if (i915->info->gen == 9)
		i915_write32(i915, GEN6_RC_VIDEO_FREQ, GEN9_FREQUENCY(rp1));
	i915_write32(i915, GEN6_RP_DOWN_TIMEOUT, rps_pm_interval(i915, 1000000000ULL));
	i915_write32(i915, GEN6_RP_IDLE_HYSTERSIS, 10);
	i915_write32(i915, GEN6_PMINTRMSK, pm_mask);
	i915_write32(i915, GEN6_RP_UP_EI, rps_pm_interval(i915, (uint64_t)ei_up * 1000));
	i915_write32(i915, GEN6_RP_UP_THRESHOLD,
		     rps_pm_interval(i915, (uint64_t)ei_up * up_pct * 10));
	i915_write32(i915, GEN6_RP_DOWN_EI, rps_pm_interval(i915, (uint64_t)ei_down * 1000));
	i915_write32(i915, GEN6_RP_DOWN_THRESHOLD,
		     rps_pm_interval(i915, (uint64_t)ei_down * down_pct * 10));
	i915_write32(i915, GEN6_RP_CONTROL,
		     (i915->info->gen > 9 ? 0 : GEN6_RP_MEDIA_TURBO) |
			     GEN6_RP_MEDIA_HW_NORMAL_MODE | GEN6_RP_MEDIA_IS_GFX |
			     GEN6_RP_ENABLE | GEN6_RP_UP_BUSY_AVG | GEN6_RP_DOWN_IDLE_AVG);
	i915_write32(i915, GEN6_RPNSWREQ, rps_freq_field(i915, want));
	i915_write32(i915, GEN6_RP_INTERRUPT_LIMITS, limits);
	(void)i915_read32(i915, GEN6_RPNSWREQ);
	i915_fw_put(i915, I915_FW_ALL);
	i915->rps_rp0 = rp0;
	i915->rps_rp1 = rp1;
	i915->rps_rpn = rpn;
	i915->rps_cur = want;
	/* The P-unit takes a moment to move the clock; what it settled on
	 * is the proof the request was understood.  Up to 50 ms. */
	uint32_t now = 0;
	for (int i = 0; i < 50; i++) {
		now = i915_rps_current_mhz(i915);
		if (now + 50 >= rps_mhz(i915, want))
			break;
		lapic_delay_us(1000);
	}
	kprintf("[drm] i915: GT clock requested at %u MHz (RPn %u, RP1 %u, RP0 %u MHz), running at %u MHz\n",
		rps_mhz(i915, want), rps_mhz(i915, rpn), rps_mhz(i915, rp1),
		rps_mhz(i915, rp0), now);
	return 0;
}

/* What the GT is running at right now, in MHz (0 when unknown). */
uint32_t i915_rps_current_mhz(struct i915_device *i915)
{
	if (i915_is_legacy(i915))
		return i915_legacy_rps_current_mhz(i915);
	if (i915->info->gen < 9)
		return 0;
	/* Gen12 moved the status (and reads zero while the GT sleeps);
	 * Meteor Lake and every part after it report the frequency in a
	 * mirror of its own */
	if (i915->gt_ip >= I915_IP(12, 70))
		return rps_mhz(i915, i915_read32(i915, MTL_MIRROR_TARGET_WP1) & MTL_CAGF_MASK);
	if (i915->info->gen_x10 >= 120)
		return rps_mhz(i915, (i915_read32(i915, GEN12_RPSTAT1) & GEN12_CAGF_MASK) >>
					     GEN12_CAGF_SHIFT);
	uint32_t st = i915_read32(i915, GEN6_RPSTAT1);
	return rps_mhz(i915, (st & GEN9_CAGF_MASK) >> GEN9_CAGF_SHIFT);
}
