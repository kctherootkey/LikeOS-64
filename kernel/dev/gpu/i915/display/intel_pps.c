// LikeOS-64 -- eDP panel power sequencing (PCH-side, Sunrise Point family).
//
// A panel must be powered in order: VDD (so its AUX channel answers),
// then panel power, then the backlight, with the delays its maker
// specifies (T1..T12 in the VBT); off in reverse.  Getting a delay
// wrong is a panel that shows nothing or one that is damaged, so the
// VBT values are used when present and conservative ones otherwise.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

/* The sequencer's registers: status, control, on delays, off delays and
 * (not on Broxton) the divisor, at the base the platform puts them. */
#define PPS(i915, off) ((i915)->display.pps_base + (off))

/* Delays in 100 us units, as the registers count them. */
struct pps_delays {
	uint32_t t1_t3; /* power up to AUX ready */
	uint32_t t8; /* power up to backlight on */
	uint32_t t9; /* backlight off to power down */
	uint32_t t10; /* power down */
	uint32_t t11_t12; /* power cycle */
};

static struct pps_delays g_pps;

static uint32_t pp_control_read(struct i915_device *i915)
{
	uint32_t v = i915_read32(i915, PPS(i915, 4));
	v &= ~PANEL_UNLOCK_MASK;
	v |= PANEL_UNLOCK_REGS;
	return v;
}

static void pp_control_write(struct i915_device *i915, uint32_t v)
{
	i915_write32(i915, PPS(i915, 4), v);
	(void)i915_read32(i915, PPS(i915, 4));
}

static void delay_100us(uint32_t units)
{
	if (units)
		lapic_delay_us(units * 100);
}

int intel_pps_init(struct i915_device *i915)
{
	struct intel_vbt *vbt = &i915->display.vbt;
	uint32_t on = i915_read32(i915, PPS(i915, 8));
	uint32_t off = i915_read32(i915, PPS(i915, 0xc));

	/* What the firmware programmed, the VBT, and the spec's maxima:
	 * the longest of each wins, so a delay is never shortened. */
	struct pps_delays hw = {
		.t1_t3 = (on & PANEL_POWER_UP_DELAY_MASK) >> PANEL_POWER_UP_DELAY_SHIFT,
		.t8 = on & PANEL_LIGHT_ON_DELAY_MASK,
		.t9 = off & PANEL_LIGHT_OFF_DELAY_MASK,
		.t10 = (off & PANEL_POWER_DOWN_DELAY_MASK) >> PANEL_POWER_DOWN_DELAY_SHIFT,
		.t11_t12 = 0,
	};
	if (i915->pch >= I915_PCH_CNP || i915->display.model == INTEL_DISPLAY_BXT) {
		uint32_t ctl = i915_read32(i915, PPS(i915, 4));
		hw.t11_t12 = ((ctl & BXT_POWER_CYCLE_DELAY_MASK) >>
			      BXT_POWER_CYCLE_DELAY_SHIFT) * 1000 / 100;
	} else {
		uint32_t div = i915_read32(i915, PPS(i915, 0x10));
		hw.t11_t12 = (div & PANEL_POWER_CYCLE_DELAY_MASK) * 1000 / 100;
	}
	struct pps_delays v = { 0, 0, 0, 0, 0 };
	if (vbt->pps.valid) {
		v.t1_t3 = vbt->pps.t1_t3;
		v.t8 = vbt->pps.t8;
		v.t9 = vbt->pps.t9;
		v.t10 = vbt->pps.t10;
		v.t11_t12 = vbt->pps.t11_t12;
	}
	/* spec maxima: T3 200 ms, T8 50 ms, T9 50 ms, T10 500 ms, T12 500 ms */
	static const struct pps_delays spec = { 2000, 500, 500, 5000, 5000 };
#define PICK(f) (hw.f > v.f ? (hw.f > spec.f ? hw.f : (v.f ? hw.f : spec.f)) : (v.f ? v.f : spec.f))
	g_pps.t1_t3 = hw.t1_t3 ? hw.t1_t3 : (v.t1_t3 ? v.t1_t3 : spec.t1_t3);
	g_pps.t8 = hw.t8 ? hw.t8 : (v.t8 ? v.t8 : spec.t8);
	g_pps.t9 = hw.t9 ? hw.t9 : (v.t9 ? v.t9 : spec.t9);
	g_pps.t10 = hw.t10 ? hw.t10 : (v.t10 ? v.t10 : spec.t10);
	g_pps.t11_t12 = hw.t11_t12 ? hw.t11_t12 : (v.t11_t12 ? v.t11_t12 : spec.t11_t12);
#undef PICK
	/* Program the sequencer with the chosen values (only the ones it
	 * carries; T12 goes into the divisor/control register). */
	uint32_t new_on = ((g_pps.t1_t3 << PANEL_POWER_UP_DELAY_SHIFT) &
			   PANEL_POWER_UP_DELAY_MASK) |
			  (g_pps.t8 & PANEL_LIGHT_ON_DELAY_MASK);
	uint32_t new_off = ((g_pps.t10 << PANEL_POWER_DOWN_DELAY_SHIFT) &
			    PANEL_POWER_DOWN_DELAY_MASK) |
			   (g_pps.t9 & PANEL_LIGHT_OFF_DELAY_MASK);
	if ((on & ~PANEL_PORT_SELECT_MASK) != new_on)
		i915_write32(i915, PPS(i915, 8), (on & PANEL_PORT_SELECT_MASK) | new_on);
	if (off != new_off)
		i915_write32(i915, PPS(i915, 0xc), new_off);
	uint32_t cycle = (g_pps.t11_t12 * 100 + 999) / 1000 + 1; /* ms/100 +1 */
	if (i915->pch >= I915_PCH_CNP || i915->display.model == INTEL_DISPLAY_BXT) {
		uint32_t ctl = pp_control_read(i915);
		ctl &= ~BXT_POWER_CYCLE_DELAY_MASK;
		ctl |= (cycle << BXT_POWER_CYCLE_DELAY_SHIFT) & BXT_POWER_CYCLE_DELAY_MASK;
		pp_control_write(i915, ctl);
	} else {
		uint32_t div = i915_read32(i915, PPS(i915, 0x10));
		div &= ~PANEL_POWER_CYCLE_DELAY_MASK;
		div |= cycle & PANEL_POWER_CYCLE_DELAY_MASK;
		i915_write32(i915, PPS(i915, 0x10), div);
	}
	i915_dbg("[drm] i915: panel power delays T1+T3 %u.%u ms, T8 %u.%u, T9 %u.%u, T10 %u.%u, T12 %u.%u\n",
		g_pps.t1_t3 / 10, g_pps.t1_t3 % 10, g_pps.t8 / 10, g_pps.t8 % 10,
		g_pps.t9 / 10, g_pps.t9 % 10, g_pps.t10 / 10, g_pps.t10 % 10,
		g_pps.t11_t12 / 10, g_pps.t11_t12 % 10);
	return 0;
}

static int pps_wait(struct i915_device *i915, uint32_t mask, uint32_t value,
		    uint32_t timeout_ms)
{
	for (uint32_t t = 0; t < timeout_ms * 10; t++) {
		if ((i915_read32(i915, PPS(i915, 0)) & mask) == value)
			return 0;
		lapic_delay_us(100);
	}
	return -ETIMEDOUT;
}

/* VDD: the panel's logic, enough for AUX.  Forced on before any DPCD
 * access; released only once the panel is fully off. */
void intel_pps_vdd_on(struct i915_device *i915, struct intel_output *o)
{
	if (o->vdd_forced)
		return;
	uint32_t ctl = pp_control_read(i915);
	int was_on = !!(ctl & EDP_FORCE_VDD) || !!(i915_read32(i915, PPS(i915, 0)) & PP_ON);
	pp_control_write(i915, ctl | EDP_FORCE_VDD);
	o->vdd_forced = 1;
	if (!was_on)
		delay_100us(g_pps.t1_t3);
}

void intel_pps_vdd_off(struct i915_device *i915, struct intel_output *o)
{
	if (!o->vdd_forced)
		return;
	uint32_t ctl = pp_control_read(i915);
	pp_control_write(i915, ctl & ~EDP_FORCE_VDD);
	o->vdd_forced = 0;
}

void intel_pps_panel_on(struct i915_device *i915, struct intel_output *o)
{
	if (i915_read32(i915, PPS(i915, 0)) & PP_ON) {
		o->panel_powered = 1;
		return;
	}
	/* the power cycle must have finished since the last off */
	pps_wait(i915, PP_CYCLE_DELAY_ACTIVE, 0, 600);
	uint32_t ctl = pp_control_read(i915);
	ctl |= PANEL_POWER_ON | PANEL_POWER_RESET;
	pp_control_write(i915, ctl);
	if (pps_wait(i915, PP_ON | PP_SEQUENCE_MASK, PP_ON | PP_SEQUENCE_NONE, 300) != 0)
		kprintf("[drm] i915: panel power on did not complete (status %08x)\n",
			i915_read32(i915, PPS(i915, 0)));
	o->panel_powered = 1;
	/* T8 before the backlight; the caller enables it after this
	 * returns, so the wait belongs here */
	delay_100us(g_pps.t8);
}

void intel_pps_panel_off(struct i915_device *i915, struct intel_output *o)
{
	uint32_t ctl = pp_control_read(i915);
	/* backlight off first (T9 elapsed by the caller) */
	ctl &= ~(PANEL_POWER_ON | EDP_BLC_ENABLE | EDP_FORCE_VDD);
	ctl |= PANEL_POWER_RESET;
	pp_control_write(i915, ctl);
	o->vdd_forced = 0;
	if (pps_wait(i915, PP_ON | PP_SEQUENCE_MASK, PP_SEQUENCE_NONE, 600) != 0)
		kprintf("[drm] i915: panel power off did not complete (status %08x)\n",
			i915_read32(i915, PPS(i915, 0)));
	o->panel_powered = 0;
	delay_100us(g_pps.t10);
}

/* The backlight enable in the sequencer (EDP_BLC_ENABLE), separate from
 * the PWM (intel_backlight.c). */
void intel_pps_backlight(struct i915_device *i915, int on)
{
	uint32_t ctl = pp_control_read(i915);
	if (on)
		ctl |= EDP_BLC_ENABLE;
	else
		ctl &= ~EDP_BLC_ENABLE;
	pp_control_write(i915, ctl);
	if (!on)
		delay_100us(g_pps.t9);
}
