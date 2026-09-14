// LikeOS-64 -- the panel backlight (PCH PWM, Sunrise Point family).
//
// A PWM in the PCH drives the panel's backlight: a frequency (from the
// VBT, in Hz, turned into a count of raw-clock cycles) and a duty cycle,
// with an enable and a polarity bit.  Brightness is the duty cycle; the
// VBT's minimum keeps the panel from being turned all the way off by a
// low setting.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/fs/sysfs.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>


/* The Broxton kind of PWM (also in the Cannon Point PCHs): a frequency
 * register and a duty register per controller, the VBT naming the
 * controller. */
static int bl_controller(struct i915_device *i915)
{
	struct intel_vbt *vbt = &i915->display.vbt;
	return (vbt->backlight_valid && vbt->backlight_controller == 1) ? 1 : 0;
}

/* The level the panel comes up at.
 *
 * Full brightness, not whatever duty cycle the firmware happened to leave in
 * the register.  What the firmware leaves is its own setting -- a value some
 * BIOS screen or a previous operating system chose, often a fraction of the
 * range -- and inheriting it means the machine boots dimmer than the panel
 * can go for no reason the user can see.  A client that wants something else
 * sets it: /sys/class/backlight/intel_backlight/brightness, or the
 * `backlight' tool over it.
 *
 * Brightest is the maximum duty cycle.  It reads the other way round on a
 * panel whose PWM the VBT marks active low, but that inversion belongs to the
 * transmitter: the polarity bit is set in BLC_PWM_PCH_CTL1 at enable time and
 * the hardware inverts the waveform, so the number here means brightness
 * either way.  (Until 2026-09-14 the VBT flag was read from the wrong bit and
 * every panel was driven inverted, which made a duty of zero the brightest
 * this driver could go; see intel_vbt_parse.c.) */
static void backlight_default_level(struct intel_display *d, uint32_t freq)
{
	if (d->bl_ready) {
		/* a resume: keep the level the panel was running at */
		if (d->bl_level > freq)
			d->bl_level = freq;
		return;
	}
	d->bl_level = freq;
	d->bl_ready = 1;
}

static int bxt_backlight_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	struct intel_vbt *vbt = &d->vbt;
	int c = bl_controller(i915);
	uint32_t ctl = i915_read32(i915, BXT_BLC_PWM_CTL(c));
	uint32_t freq = i915_read32(i915, BXT_BLC_PWM_FREQ(c));
	uint32_t duty = i915_read32(i915, BXT_BLC_PWM_DUTY(c));

	if (!freq && vbt->backlight_valid && vbt->backlight_pwm_hz)
		freq = d->rawclk_khz * 1000 / vbt->backlight_pwm_hz;
	if (!freq)
		freq = 1000;
	d->bl_max = freq;
	d->bl_min = vbt->backlight_valid ? (freq * vbt->backlight_min + 127) / 255 : 0;
	if (d->bl_min > freq / 4)
		d->bl_min = freq / 4;
	backlight_default_level(d, freq);
	d->bl_enabled = !!(ctl & BXT_BLC_PWM_ENABLE);
	i915_dbg("[drm] i915: backlight PWM %d period %u, level %u of %u (firmware had %u)%s, %s\n",
		 c, freq, d->bl_level, d->bl_max, duty,
		 (vbt->backlight_valid && vbt->backlight_active_low) ? ", active low" : "",
		 d->bl_enabled ? "on" : "off");
	return 0;
}

int intel_backlight_init(struct i915_device *i915)
{
	struct intel_display *d = &i915->display;
	struct intel_vbt *vbt = &d->vbt;
	if (d->bl_bxt)
		return bxt_backlight_init(i915);
	uint32_t ctl1 = i915_read32(i915, BLC_PWM_PCH_CTL1);
	uint32_t ctl2 = i915_read32(i915, BLC_PWM_PCH_CTL2);
	uint32_t freq = ctl2 >> 16;
	uint32_t duty = ctl2 & 0xffff;

	/* The PWM period in raw-clock cycles: what the firmware set, else
	 * from the VBT frequency (the PCH counts 128 or 16 raw clocks per
	 * unit, chosen by the granularity chicken bit). */
	if (!freq && vbt->backlight_valid && vbt->backlight_pwm_hz) {
		uint32_t gran = (i915_read32(i915, SOUTH_CHICKEN2) & LPT_PWM_GRANULARITY) ? 16 : 128;
		freq = d->rawclk_khz * 1000 / (gran * vbt->backlight_pwm_hz);
	}
	if (!freq)
		freq = 1000; /* something sane if the VBT says nothing */
	d->bl_max = freq;
	d->bl_min = vbt->backlight_valid ? (freq * vbt->backlight_min + 127) / 255 : 0;
	if (d->bl_min > freq / 4)
		d->bl_min = freq / 4;
	backlight_default_level(d, freq);
	d->bl_enabled = !!(ctl1 & BLM_PCH_PWM_ENABLE);
	i915_dbg("[drm] i915: backlight PWM period %u, level %u of %u (firmware had %u)%s, %s\n",
		freq, d->bl_level, d->bl_max, duty,
		(vbt->backlight_valid && vbt->backlight_active_low) ? ", active low" : "",
		d->bl_enabled ? "on" : "off");
	return 0;
}

uint32_t intel_backlight_max(struct i915_device *i915)
{
	return i915->display.bl_max;
}

uint32_t intel_backlight_get(struct i915_device *i915)
{
	return i915->display.bl_level;
}

int intel_backlight_set(struct i915_device *i915, uint32_t level)
{
	struct intel_display *d = &i915->display;

	if (level > d->bl_max)
		level = d->bl_max;
	if (level && level < d->bl_min)
		level = d->bl_min;
	d->bl_level = level;
	if (d->bl_bxt) {
		i915_write32(i915, BXT_BLC_PWM_DUTY(bl_controller(i915)), level);
		return 0;
	}
	uint32_t ctl2 = i915_read32(i915, BLC_PWM_PCH_CTL2) & 0xffff0000u;
	i915_write32(i915, BLC_PWM_PCH_CTL2, ctl2 | level);
	return 0;
}

void intel_backlight_enable(struct i915_device *i915, struct intel_output *o)
{
	struct intel_display *d = &i915->display;
	struct intel_vbt *vbt = &d->vbt;
	(void)o;

	if (d->bl_bxt) {
		int c = bl_controller(i915);
		uint32_t ctl = i915_read32(i915, BXT_BLC_PWM_CTL(c));
		if (ctl & BXT_BLC_PWM_ENABLE)
			i915_write32(i915, BXT_BLC_PWM_CTL(c), ctl & ~BXT_BLC_PWM_ENABLE);
		i915_write32(i915, BXT_BLC_PWM_FREQ(c), d->bl_max);
		i915_write32(i915, BXT_BLC_PWM_DUTY(c), d->bl_level);
		ctl &= ~BXT_BLC_PWM_POLARITY;
		if (vbt->backlight_valid && vbt->backlight_active_low)
			ctl |= BXT_BLC_PWM_POLARITY;
		i915_write32(i915, BXT_BLC_PWM_CTL(c), ctl);
		(void)i915_read32(i915, BXT_BLC_PWM_CTL(c));
		i915_write32(i915, BXT_BLC_PWM_CTL(c), ctl | BXT_BLC_PWM_ENABLE);
		(void)i915_read32(i915, BXT_BLC_PWM_CTL(c));
		intel_pps_backlight(i915, 1);
		d->bl_enabled = 1;
		return;
	}
	uint32_t ctl1 = i915_read32(i915, BLC_PWM_PCH_CTL1);
	if (ctl1 & BLM_PCH_PWM_ENABLE) {
		/* firmware left it on: disable before reprogramming */
		i915_write32(i915, BLC_PWM_PCH_CTL1, ctl1 & ~BLM_PCH_PWM_ENABLE);
	}
	uint32_t ctl2 = (d->bl_max << 16) | d->bl_level;
	i915_write32(i915, BLC_PWM_PCH_CTL2, ctl2);
	ctl1 &= ~BLM_PCH_POLARITY;
	if (vbt->backlight_valid && vbt->backlight_active_low)
		ctl1 |= BLM_PCH_POLARITY;
	i915_write32(i915, BLC_PWM_PCH_CTL1, ctl1);
	(void)i915_read32(i915, BLC_PWM_PCH_CTL1);
	i915_write32(i915, BLC_PWM_PCH_CTL1, ctl1 | BLM_PCH_PWM_ENABLE);
	(void)i915_read32(i915, BLC_PWM_PCH_CTL1);
	/* and the sequencer's enable, which gates the panel's BL pin */
	intel_pps_backlight(i915, 1);
	d->bl_enabled = 1;
}

void intel_backlight_disable(struct i915_device *i915, struct intel_output *o)
{
	struct intel_display *d = &i915->display;
	(void)o;

	intel_pps_backlight(i915, 0);
	if (d->bl_bxt) {
		int c = bl_controller(i915);
		i915_write32(i915, BXT_BLC_PWM_DUTY(c), 0);
		uint32_t ctl = i915_read32(i915, BXT_BLC_PWM_CTL(c));
		i915_write32(i915, BXT_BLC_PWM_CTL(c), ctl & ~BXT_BLC_PWM_ENABLE);
		(void)i915_read32(i915, BXT_BLC_PWM_CTL(c));
		d->bl_enabled = 0;
		return;
	}
	uint32_t ctl2 = i915_read32(i915, BLC_PWM_PCH_CTL2) & 0xffff0000u;
	i915_write32(i915, BLC_PWM_PCH_CTL2, ctl2);
	uint32_t ctl1 = i915_read32(i915, BLC_PWM_PCH_CTL1);
	i915_write32(i915, BLC_PWM_PCH_CTL1, ctl1 & ~BLM_PCH_PWM_ENABLE);
	(void)i915_read32(i915, BLC_PWM_PCH_CTL1);
	d->bl_enabled = 0;
}

/* ---- the control files ----------------------------------------------------- */
/*
 * /sys/class/backlight/intel_backlight/ is how a desktop reaches the
 * backlight: brightness (read/write, 0..max_brightness),
 * actual_brightness, max_brightness, bl_power (0 on, 4 off) and type
 * ("raw": the driver drives the PWM itself).  A display server that
 * knows the backlight class finds it there and offers it as an output
 * property; backlight(1) does the same from the shell.
 */

static long parse_uint(const char *buf, long len, uint32_t *out)
{
	uint32_t v = 0;
	long i = 0;
	while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
		i++;
	if (i == len || buf[i] < '0' || buf[i] > '9')
		return -EINVAL;
	while (i < len && buf[i] >= '0' && buf[i] <= '9') {
		if (v > 400000000u)
			return -ERANGE;
		v = v * 10 + (uint32_t)(buf[i] - '0');
		i++;
	}
	while (i < len && (buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t'))
		i++;
	if (i != len)
		return -EINVAL;
	*out = v;
	return 0;
}

static struct intel_output *edp_output(struct i915_device *i915)
{
	for (int i = 0; i < i915->display.nout; i++)
		if (i915->display.outputs[i].present && i915->display.outputs[i].is_edp)
			return &i915->display.outputs[i];
	return NULL;
}

static long bl_show_brightness(struct pfs_node *n, char *buf, long cap)
{
	struct i915_device *i915 = n->arg;
	return pfs_printf(buf, cap, 0, "%u\n", intel_backlight_get(i915));
}

static long bl_show_actual(struct pfs_node *n, char *buf, long cap)
{
	struct i915_device *i915 = n->arg;
	uint32_t duty = i915_read32(i915, BLC_PWM_PCH_CTL2) & 0xffff;
	return pfs_printf(buf, cap, 0, "%u\n", duty);
}

static long bl_show_max(struct pfs_node *n, char *buf, long cap)
{
	struct i915_device *i915 = n->arg;
	return pfs_printf(buf, cap, 0, "%u\n", intel_backlight_max(i915));
}

static long bl_show_power(struct pfs_node *n, char *buf, long cap)
{
	struct i915_device *i915 = n->arg;
	return pfs_printf(buf, cap, 0, "%u\n", i915->display.bl_enabled ? 0 : 4);
}

static long bl_show_type(struct pfs_node *n, char *buf, long cap)
{
	(void)n;
	return pfs_printf(buf, cap, 0, "raw\n");
}

static long bl_store_brightness(struct pfs_node *n, const char *buf, long len)
{
	struct i915_device *i915 = n->arg;
	uint32_t v;
	long rc = parse_uint(buf, len, &v);
	if (rc)
		return rc;
	if (v > intel_backlight_max(i915))
		return -EINVAL;
	intel_backlight_set(i915, v);
	return len;
}

static long bl_store_power(struct pfs_node *n, const char *buf, long len)
{
	struct i915_device *i915 = n->arg;
	uint32_t v;
	long rc = parse_uint(buf, len, &v);
	if (rc)
		return rc;
	struct intel_output *o = edp_output(i915);
	if (!o)
		return -ENODEV;
	if (v == 0) {
		if (o->active && !i915->display.bl_enabled)
			intel_backlight_enable(i915, o);
	} else if (v == 4 || v == 1) {
		if (i915->display.bl_enabled)
			intel_backlight_disable(i915, o);
	} else {
		return -EINVAL;
	}
	return len;
}

int intel_backlight_sysfs_init(struct i915_device *i915)
{
	char base[128];

	if (!edp_output(i915))
		return 0;
	int rc = sysfs_add_class_device("backlight", "intel_backlight", i915->pci, base,
					sizeof(base));
	if (rc)
		return rc;
	sysfs_add_attr(base, "brightness", bl_show_brightness, bl_store_brightness, i915, 0);
	sysfs_add_attr(base, "actual_brightness", bl_show_actual, NULL, i915, 0);
	sysfs_add_attr(base, "max_brightness", bl_show_max, NULL, i915, 0);
	sysfs_add_attr(base, "bl_power", bl_show_power, bl_store_power, i915, 0);
	sysfs_add_attr(base, "type", bl_show_type, NULL, i915, 0);
	i915_dbg("[drm] i915: backlight control at /sys/class/backlight/intel_backlight\n");
	return 0;
}
