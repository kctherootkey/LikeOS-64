// LikeOS -- render P-states and render standby before Broadwell.
//
// Sandy Bridge to Haswell request their GT frequency in GEN6_RPNSWREQ
// and let the hardware evaluate how busy the GT is: it raises an
// interrupt when the busyness crosses the up or down threshold over an
// evaluation interval, or when the GT has been idle long enough.  The
// driver moves the request one bin, then two, then four in the
// direction asked, between the lowest frequency and RP0, and widens or
// narrows the thresholds as the frequency enters the low, middle or
// high band.  Valleyview takes its request through the P-unit; there
// the GT is simply run at its highest frequency.
//
// Render standby (RC6) is switched on with the timings the hardware
// wants, where the parts enable it by default: Sandy Bridge, Ivy
// Bridge, Haswell, and Valleyview when the firmware has set up its power
// context.  Ironlake's frequency control goes through the memory
// controller's IPS and is left as the firmware set it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2008-2024 Intel Corporation

#include <kernel/dev/gpu/i915/i915_legacy_priv.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

enum { LOW_POWER, BETWEEN, HIGH_POWER };

#define RPS_UP_THRESHOLD 95 /* percent */
#define RPS_DOWN_THRESHOLD 85

static spinlock_t g_rps_lock;
static uint8_t g_max_soft, g_min_soft;

/* ---- frequencies ------------------------------------------------------------------ */

/* MHz of a frequency value: 50 MHz bins on Gen6/7; Valleyview's depend
 * on its GPLL and are reported as the raw value. */
static uint32_t rps_mhz(struct i915_device *i915, uint32_t val)
{
	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW))
		return val;
	return val * 50;
}

/* Time in the PM unit's intervals (1.28 us here: the 12.5 MHz clock
 * divided by 16).  Sandy Bridge wants multiples of 25, or some machines
 * slow to a crawl around a GPU hang. */
static uint32_t ns_to_pm_interval(struct i915_device *i915, uint64_t ns)
{
	uint64_t clocks = ns * 125 / 10000; /* 12.5 MHz */
	uint64_t val = (clocks + 15) / 16;

	if (leg_gen(i915) == 6)
		val = (val + 24) / 25 * 25;
	return (uint32_t)val;
}

static void rps_set_power(struct i915_device *i915, int new_power)
{
	uint32_t ei_up = 0, ei_down = 0;

	if (new_power == g_leg.power_mode)
		return;
	/* the units here are 1.28 us, not quite microseconds */
	switch (new_power) {
	case LOW_POWER:
		ei_up = 16000;
		ei_down = 32000;
		break;
	case BETWEEN:
		ei_up = 13000;
		ei_down = 32000;
		break;
	case HIGH_POWER:
		ei_up = 10000;
		ei_down = 32000;
		break;
	}
	i915_write32(i915, GEN6_RP_UP_EI, ns_to_pm_interval(i915, (uint64_t)ei_up * 1000));
	i915_write32(i915, GEN6_RP_UP_THRESHOLD,
		     ns_to_pm_interval(i915, (uint64_t)ei_up * RPS_UP_THRESHOLD * 10));
	i915_write32(i915, GEN6_RP_DOWN_EI, ns_to_pm_interval(i915, (uint64_t)ei_down * 1000));
	i915_write32(i915, GEN6_RP_DOWN_THRESHOLD,
		     ns_to_pm_interval(i915, (uint64_t)ei_down * RPS_DOWN_THRESHOLD * 10));
	i915_write32(i915, GEN6_RP_CONTROL,
		     GEN6_RP_MEDIA_TURBO | GEN6_RP_MEDIA_HW_NORMAL_MODE | GEN6_RP_MEDIA_IS_GFX |
			     GEN6_RP_ENABLE | GEN6_RP_UP_BUSY_AVG | GEN6_RP_DOWN_IDLE_AVG);
	g_leg.power_mode = new_power;
}

static void gen6_rps_set_thresholds(struct i915_device *i915, uint8_t val)
{
	int new_power = g_leg.power_mode;

	switch (g_leg.power_mode) {
	case LOW_POWER:
		if (val > g_leg.rpe + 1 && val > g_leg.cur_freq)
			new_power = BETWEEN;
		break;
	case BETWEEN:
		if (val <= g_leg.rpe && val < g_leg.cur_freq)
			new_power = LOW_POWER;
		else if (val >= g_leg.rp0 && val > g_leg.cur_freq)
			new_power = HIGH_POWER;
		break;
	case HIGH_POWER:
		if (val < (g_leg.rp1 + g_leg.rp0) >> 1 && val < g_leg.cur_freq)
			new_power = BETWEEN;
		break;
	default:
		break;
	}
	/* the bins at the ends are special */
	if (val <= g_min_soft)
		new_power = LOW_POWER;
	if (val >= g_max_soft)
		new_power = HIGH_POWER;
	rps_set_power(i915, new_power);
}

static uint32_t rps_limits(uint8_t val)
{
	/* the down limit only at the bottom: a down interrupt in the
	 * moment the GT leaves RC6 at its lowest clock would be lost */
	uint32_t limits = (uint32_t)g_max_soft << 24;
	if (val <= g_min_soft)
		limits |= (uint32_t)g_min_soft << 16;
	return limits;
}

static uint32_t rps_pm_mask(uint8_t val)
{
	uint32_t mask = 0;

	if (val > g_min_soft)
		mask |= GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_DOWN_THRESHOLD | GEN6_PM_RP_DOWN_TIMEOUT;
	if (val < g_max_soft)
		mask |= GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_UP_THRESHOLD;
	mask &= g_leg.pm_events;
	/* SNB, IVB and HSW can hang on a looping batch with the up-EI
	 * event masked */
	return ~mask & ~(uint32_t)GEN6_PM_RP_UP_EI_EXPIRED;
}

static void gen6_rps_write(struct i915_device *i915, uint8_t val)
{
	uint32_t swreq;

	if (leg_is(i915, I915_PLATFORM_HASWELL))
		swreq = HSW_FREQUENCY(val);
	else
		swreq = GEN6_FREQUENCY(val) | GEN6_OFFSET(0) | GEN6_AGGRESSIVE_TURBO;
	i915_write32(i915, GEN6_RPNSWREQ, swreq);
}

/* The request moved to `val', the thresholds and the interrupt limits
 * with it. */
static void rps_set(struct i915_device *i915, uint8_t val)
{
	gen6_rps_write(i915, val);
	gen6_rps_set_thresholds(i915, val);
	if (g_leg.rps_dynamic) {
		i915_write32(i915, GEN6_RP_INTERRUPT_LIMITS, rps_limits(val));
		i915_write32(i915, GEN6_PMINTRMSK, rps_pm_mask(val));
	}
	g_leg.cur_freq = val;
}

/* From the interrupt handler: the events the evaluation raised. */
void leg_rps_irq(struct i915_device *i915, uint32_t pm_iir)
{
	uint32_t events = pm_iir & g_leg.pm_events;
	int adj, new_freq;
	uint64_t fl;

	if (!events || !g_leg.rps_dynamic)
		return;
	spin_lock_irqsave(&g_rps_lock, &fl);
	adj = g_leg.last_adj;
	new_freq = g_leg.cur_freq;
	if (events & GEN6_PM_RP_UP_THRESHOLD) {
		adj = adj > 0 ? adj * 2 : 1;
		if (new_freq >= g_max_soft)
			adj = 0;
	} else if (events & GEN6_PM_RP_DOWN_TIMEOUT) {
		if (g_leg.cur_freq > g_leg.rpe)
			new_freq = g_leg.rpe;
		else if (g_leg.cur_freq > g_min_soft)
			new_freq = g_min_soft;
		adj = 0;
	} else if (events & GEN6_PM_RP_DOWN_THRESHOLD) {
		adj = adj < 0 ? adj * 2 : -1;
		if (new_freq <= g_min_soft)
			adj = 0;
	} else {
		adj = 0;
	}
	new_freq += adj;
	if (new_freq < g_min_soft)
		new_freq = g_min_soft;
	if (new_freq > g_max_soft)
		new_freq = g_max_soft;
	if (new_freq != g_leg.cur_freq)
		rps_set(i915, (uint8_t)new_freq);
	g_leg.last_adj = adj;
	spin_unlock_irqrestore(&g_rps_lock, fl);
}

/* ---- Valleyview through the P-unit ------------------------------------------------- */

static int vlv_rps_init(struct i915_device *i915)
{
	uint32_t val = 0, lo = 0, hi = 0;

	if (i915_legacy_vlv_nc_read(i915, IOSF_NC_FB_GFX_FREQ_FUSE, &val) != 0)
		return -EIO;
	g_leg.rp0 = (uint8_t)((val & FB_GFX_MAX_FREQ_FUSE_MASK) >> FB_GFX_MAX_FREQ_FUSE_SHIFT);
	if (g_leg.rp0 > 0xea)
		g_leg.rp0 = 0xea;
	g_leg.rp1 = (uint8_t)((val & FB_GFX_FGUARANTEED_FREQ_FUSE_MASK) >>
			      FB_GFX_FGUARANTEED_FREQ_FUSE_SHIFT);
	if (i915_legacy_vlv_nc_read(i915, IOSF_NC_FB_GFX_FMAX_FUSE_LO, &lo) == 0 &&
	    i915_legacy_vlv_nc_read(i915, IOSF_NC_FB_GFX_FMAX_FUSE_HI, &hi) == 0)
		g_leg.rpe = (uint8_t)(((lo & FB_FMAX_VMIN_FREQ_LO_MASK) >> FB_FMAX_VMIN_FREQ_LO_SHIFT) |
				      ((hi & FB_FMAX_VMIN_FREQ_HI_MASK) << 5));
	if (i915_legacy_vlv_punit_read(i915, PUNIT_REG_GPU_LFM, &val) != 0)
		return -EIO;
	/* the P-unit refuses anything below 0xc0 in GPLL mode */
	g_leg.rpn = (uint8_t)((val & 0xff) < 0xc0 ? 0xc0 : (val & 0xff));
	g_leg.max_freq = g_leg.rp0;
	g_leg.min_freq = g_leg.rpn;
	if (g_leg.max_freq <= g_leg.min_freq)
		return -ENODEV;

	i915_fw_get(i915, I915_FW_ALL);
	i915_write32(i915, GEN6_RP_DOWN_TIMEOUT, 1000000);
	i915_write32(i915, GEN6_RP_UP_THRESHOLD, 59400);
	i915_write32(i915, GEN6_RP_DOWN_THRESHOLD, 245000);
	i915_write32(i915, GEN6_RP_UP_EI, 66000);
	i915_write32(i915, GEN6_RP_DOWN_EI, 350000);
	i915_write32(i915, GEN6_RP_IDLE_HYSTERSIS, 10);
	i915_write32(i915, GEN6_RP_CONTROL,
		     GEN6_RP_MEDIA_TURBO | GEN6_RP_MEDIA_HW_NORMAL_MODE | GEN6_RP_MEDIA_IS_GFX |
			     GEN6_RP_ENABLE | GEN6_RP_UP_BUSY_AVG | GEN6_RP_DOWN_IDLE_CONT);
	i915_fw_put(i915, I915_FW_ALL);
	/* a fixed bias between the cores and the SoC */
	(void)i915_legacy_vlv_punit_write(i915, VLV_TURBO_SOC_OVERRIDE,
					  VLV_OVERRIDE_EN | VLV_SOC_TDP_EN | VLV_BIAS_CPU_125_SOC_875);
	if (i915_legacy_vlv_punit_read(i915, PUNIT_REG_GPU_FREQ_STS, &val) == 0 && !(val & GPLLENABLE))
		kprintf("[drm] i915: the GPLL is off; the frequency request may not take\n");
	/* the request: the highest frequency, which the P-unit lowers by
	 * itself when it must */
	if (i915_legacy_vlv_punit_write(i915, PUNIT_REG_GPU_FREQ_REQ, g_leg.rp0) != 0)
		return -EIO;
	g_leg.cur_freq = g_leg.rp0;
	kprintf("[drm] i915: GT frequency requested at %u (min %u, efficient %u, guaranteed %u)\n",
		g_leg.rp0, g_leg.rpn, g_leg.rpe, g_leg.rp1);
	return 0;
}

/* ---- render standby ----------------------------------------------------------------- */

static void gen6_rc6_enable(struct i915_device *i915)
{
	uint32_t rc6vids = 0, rc6_mask;

	i915_write32(i915, GEN6_RC1_WAKE_RATE_LIMIT, 1000 << 16);
	i915_write32(i915, GEN6_RC6_WAKE_RATE_LIMIT, 40 << 16 | 30);
	i915_write32(i915, GEN6_RC6pp_WAKE_RATE_LIMIT, 30);
	i915_write32(i915, GEN6_RC_EVALUATION_INTERVAL, 125000);
	i915_write32(i915, GEN6_RC_IDLE_HYSTERSIS, 25);
	for (int i = 0; i < I915_NUM_ENGINES; i++)
		if (i915->engines[i].present)
			i915_write32(i915, RING_MAX_IDLE(i915->engines[i].mmio_base), 10);
	i915_write32(i915, GEN6_RC_SLEEP, 0);
	i915_write32(i915, GEN6_RC1e_THRESHOLD, 1000);
	i915_write32(i915, GEN6_RC6_THRESHOLD, 50000);
	i915_write32(i915, GEN6_RC6p_THRESHOLD, 150000);
	i915_write32(i915, GEN6_RC6pp_THRESHOLD, 64000); /* unused */

	/* deep RC6 only on Ivy Bridge: Sandy Bridge has it but it
	 * misbehaves, Haswell removed it */
	rc6_mask = GEN6_RC_CTL_RC6_ENABLE;
	if (leg_is(i915, I915_PLATFORM_IVYBRIDGE))
		rc6_mask |= GEN6_RC_CTL_RC6p_ENABLE;
	g_leg.rc6_ctl = rc6_mask | GEN6_RC_CTL_EI_MODE(1) | GEN6_RC_CTL_HW_ENABLE;

	if (leg_pcode_read(i915, GEN6_PCODE_READ_RC6VIDS, &rc6vids, NULL) == 0 &&
	    leg_gen(i915) == 6 && GEN6_DECODE_RC6_VID(rc6vids & 0xff) < 450) {
		kprintf("[drm] i915: the firmware set the RC6 voltage too low (%u mV); 450 mV it is\n",
			GEN6_DECODE_RC6_VID(rc6vids & 0xff));
		rc6vids &= 0xffff00;
		rc6vids |= GEN6_ENCODE_RC6_VID(450);
		if (leg_pcode_write(i915, GEN6_PCODE_WRITE_RC6VIDS, rc6vids) != 0)
			kprintf("[drm] i915: could not correct the RC6 voltage\n");
	}
}

static int vlv_rc6_enable(struct i915_device *i915)
{
	/* The power context the GT saves into in RC6 lives in stolen
	 * memory; the firmware normally sets it up, and where it has not
	 * RC6 stays off. */
	if (!(i915_read32(i915, VLV_PCBR) & ~0xfffu)) {
		kprintf("[drm] i915: no RC6 power context from the firmware; render standby off\n");
		return -ENODEV;
	}
	i915_write32(i915, GEN6_RC6_WAKE_RATE_LIMIT, 0x00280000);
	i915_write32(i915, GEN6_RC_EVALUATION_INTERVAL, 125000);
	i915_write32(i915, GEN6_RC_IDLE_HYSTERSIS, 25);
	for (int i = 0; i < I915_NUM_ENGINES; i++)
		if (i915->engines[i].present)
			i915_write32(i915, RING_MAX_IDLE(i915->engines[i].mmio_base), 10);
	i915_write32(i915, GEN6_RC6_THRESHOLD, 0x557);
	/* the residency counters */
	i915_write32(i915, VLV_COUNTER_CONTROL,
		     LEG_MASKED_EN(VLV_COUNT_RANGE_HIGH | VLV_MEDIA_RC0_COUNT_EN |
				   VLV_RENDER_RC0_COUNT_EN | VLV_MEDIA_RC6_COUNT_EN |
				   VLV_RENDER_RC6_COUNT_EN));
	g_leg.rc6_ctl = GEN7_RC_CTL_TO_MODE | VLV_RC_CTL_CTX_RST_PARALLEL;
	return 0;
}

static void rc6_disable(struct i915_device *i915)
{
	i915_fw_get(i915, I915_FW_ALL);
	i915_write32(i915, GEN6_RC_CONTROL, 0);
	i915_write32(i915, GEN6_RC_STATE, 0);
	i915_fw_put(i915, I915_FW_ALL);
}

static void rc6_enable(struct i915_device *i915)
{
	int rc = 0;

	rc6_disable(i915);
	i915_fw_get(i915, I915_FW_ALL);
	g_leg.rc6_ctl = 0;
	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW))
		rc = vlv_rc6_enable(i915);
	else
		gen6_rc6_enable(i915);
	/* the hardware's own timers decide when to enter it */
	if (rc == 0 && g_leg.rc6_ctl)
		i915_write32(i915, GEN6_RC_CONTROL, g_leg.rc6_ctl);
	i915_fw_put(i915, I915_FW_ALL);
}

/* ---- bring-up ------------------------------------------------------------------------ */

int i915_legacy_rps_init(struct i915_device *i915)
{
	uint32_t cap;

	if (leg_gen(i915) < 6)
		return 0;
	spinlock_init(&g_rps_lock, "i915_rps");
	g_leg.rps_dynamic = 0;
	g_leg.pm_events = 0;
	rc6_enable(i915);
	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW)) {
		int rc = vlv_rps_init(i915);
		if (rc)
			kprintf("[drm] i915: the P-unit did not answer; the GT clock stays as found\n");
		g_leg.rps_enabled = rc == 0;
		i915->rps_rp0 = g_leg.rp0;
		i915->rps_rp1 = g_leg.rp1;
		i915->rps_rpn = g_leg.rpn;
		i915->rps_cur = g_leg.cur_freq;
		return rc;
	}

	cap = i915_read32(i915, GEN6_RP_STATE_CAP);
	g_leg.rp0 = cap & 0xff;
	g_leg.rp1 = (cap >> 8) & 0xff;
	g_leg.rpn = (cap >> 16) & 0xff;
	if (!g_leg.rp0 || !g_leg.rpn || g_leg.rp0 < g_leg.rpn) {
		kprintf("[drm] i915: no P-state capabilities (%08x); the clock stays as found\n", cap);
		return -ENODEV;
	}
	g_leg.min_freq = g_leg.rpn;
	g_leg.max_freq = g_leg.rp0;
	g_leg.rpe = g_leg.rp1;
	if (leg_is(i915, I915_PLATFORM_HASWELL)) {
		uint32_t ddcc = 0;
		if (leg_pcode_read(i915, HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL, &ddcc, NULL) == 0) {
			uint32_t e = (ddcc >> 8) & 0xff;
			if (e < g_leg.min_freq)
				e = g_leg.min_freq;
			if (e > g_leg.max_freq)
				e = g_leg.max_freq;
			g_leg.rpe = (uint8_t)e;
		}
	}
	g_max_soft = g_leg.max_freq;
	g_min_soft = g_leg.min_freq;
	/* the overclocking limit, where the part has one, is the most a
	 * boost may ask for; the governor stays at RP0 */
	{
		uint32_t params = 0;
		if (leg_pcode_read(i915, GEN6_READ_OC_PARAMS, &params, NULL) == 0 &&
		    (params & (1u << 31)))
			g_leg.max_freq = params & 0xff;
	}

	i915_fw_get(i915, I915_FW_ALL);
	/* power down if completely idle for over 50 ms */
	i915_write32(i915, GEN6_RP_DOWN_TIMEOUT, 50000);
	i915_write32(i915, GEN6_RP_IDLE_HYSTERSIS, 10);
	g_leg.pm_events = GEN6_PM_RP_UP_THRESHOLD | GEN6_PM_RP_DOWN_THRESHOLD |
			  GEN6_PM_RP_DOWN_TIMEOUT;
	g_leg.power_mode = -1;
	g_leg.last_adj = 0;
	g_leg.cur_freq = g_leg.min_freq;
	g_leg.rps_dynamic = 1;
	/* from the efficient frequency on, the evaluation moves it */
	rps_set(i915, g_leg.rpe > g_min_soft ? g_leg.rpe : g_min_soft);
	/* the P-state events into the PM unit's interrupts */
	g_leg.pm_imr &= ~g_leg.pm_events;
	i915_write32(i915, GEN6_PMIIR, g_leg.pm_events);
	i915_write32(i915, GEN6_PMIER, i915_read32(i915, GEN6_PMIER) | g_leg.pm_events);
	i915_write32(i915, GEN6_PMIMR, i915_read32(i915, GEN6_PMIMR) & ~g_leg.pm_events);
	i915_write32(i915, GEN6_PMINTRMSK, rps_pm_mask(g_leg.cur_freq));
	i915_fw_put(i915, I915_FW_ALL);
	g_leg.rps_enabled = 1;

	i915->rps_rp0 = g_leg.rp0;
	i915->rps_rp1 = g_leg.rp1;
	i915->rps_rpn = g_leg.rpn;
	i915->rps_cur = g_leg.cur_freq;
	kprintf("[drm] i915: GT clock %u-%u MHz (efficient %u, RP1 %u MHz), starting at %u MHz\n",
		rps_mhz(i915, g_leg.rpn), rps_mhz(i915, g_leg.rp0), rps_mhz(i915, g_leg.rpe),
		rps_mhz(i915, g_leg.rp1), rps_mhz(i915, g_leg.cur_freq));
	return 0;
}

void i915_legacy_rps_fini(struct i915_device *i915)
{
	if (leg_gen(i915) < 6)
		return;
	if (g_leg.rps_enabled && !leg_is(i915, I915_PLATFORM_VALLEYVIEW)) {
		uint64_t fl;
		spin_lock_irqsave(&g_rps_lock, &fl);
		g_leg.rps_dynamic = 0;
		spin_unlock_irqrestore(&g_rps_lock, fl);
		i915_write32(i915, GEN6_PMINTRMSK, ~0u);
		i915_write32(i915, GEN6_PMIMR, i915_read32(i915, GEN6_PMIMR) | GEN6_PM_RPS_EVENTS);
		i915_write32(i915, GEN6_PMIER, i915_read32(i915, GEN6_PMIER) & ~GEN6_PM_RPS_EVENTS);
		i915_write32(i915, GEN6_RP_CONTROL, 0);
	}
	g_leg.pm_events = 0;
	g_leg.rps_enabled = 0;
	rc6_disable(i915);
}

uint32_t i915_legacy_rps_current_mhz(struct i915_device *i915)
{
	uint32_t st;

	if (leg_gen(i915) < 6)
		return 0;
	if (leg_is(i915, I915_PLATFORM_VALLEYVIEW)) {
		uint32_t v = 0;
		if (i915_legacy_vlv_punit_read(i915, PUNIT_REG_GPU_FREQ_STS, &v) != 0)
			return 0;
		return (v >> 8) & 0xff;
	}
	st = i915_read32(i915, GEN6_RPSTAT1);
	if (leg_is(i915, I915_PLATFORM_HASWELL))
		return rps_mhz(i915, (st & HSW_CAGF_MASK) >> HSW_CAGF_SHIFT);
	return rps_mhz(i915, (st & GEN6_CAGF_MASK) >> GEN6_CAGF_SHIFT);
}
