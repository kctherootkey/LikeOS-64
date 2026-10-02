// LikeOS -- the analog VGA output of the Intel parts before DDI.
//
// Every non-DDI part but Cherryview has a VGA DAC: ADPA in the display
// block of the GMCH parts (gen2 to G4X, and Valleyview, where it sits in
// the moved display block and exists only when the VBT says so), PCH_ADPA
// on the PCH of Ironlake to Ivy Bridge, fed over FDI.  The DAC turns the
// pipe's pixels into RGB voltages and passes the syncs through, each of
// which can be held off for the old power-saving states.  A monitor is
// found three ways: the DAC's own load sensing, which the 945G and later
// can run on demand (a forced hotplug cycle reporting whether the blue and
// green lines are terminated), the EDID on the VGA DDC pins, or, on the
// oldest parts, by lighting a pipe with a border colour and watching the
// DAC's sense comparator in the VGA status register.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2007 Intel Corporation

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The hotplug sampling the PCH and Valleyview DAC runs on its own. */
#define CRT_ADPA_HOTPLUG_BITS                                                   \
	(ADPA_CRT_HOTPLUG_ENABLE | ADPA_CRT_HOTPLUG_PERIOD_128 |                 \
	 ADPA_CRT_HOTPLUG_WARMUP_10MS | ADPA_CRT_HOTPLUG_SAMPLE_4S |            \
	 ADPA_CRT_HOTPLUG_VOLTAGE_50 | ADPA_CRT_HOTPLUG_VOLREF_325MV)
#define CRT_ADPA_HOTPLUG_PERIOD_MASK (1u << 22)
#define CRT_ADPA_HOTPLUG_WARMUP_MASK (1u << 21)
#define CRT_ADPA_HOTPLUG_SAMPLE_MASK (1u << 20)
#define CRT_ADPA_HOTPLUG_VOLTAGE_MASK (3u << 18)
#define CRT_ADPA_HOTPLUG_VOLREF_MASK (1u << 17)
#define CRT_ADPA_HOTPLUG_MASK                                                   \
	(ADPA_CRT_HOTPLUG_MONITOR_MASK | ADPA_CRT_HOTPLUG_ENABLE |               \
	 CRT_ADPA_HOTPLUG_PERIOD_MASK | CRT_ADPA_HOTPLUG_WARMUP_MASK |          \
	 CRT_ADPA_HOTPLUG_SAMPLE_MASK | CRT_ADPA_HOTPLUG_VOLTAGE_MASK |         \
	 CRT_ADPA_HOTPLUG_VOLREF_MASK | ADPA_CRT_HOTPLUG_FORCE_TRIGGER)
#define CRT_ADPA_PIPE_SEL_SHIFT 30
#define CRT_ADPA_PIPE_SEL_SHIFT_CPT 29

/* The VGA input status register 0, mirrored in MMIO: bit 4 is the DAC's
 * sense comparator. */
#define CRT_VGA_IS0_R 0x3c2u
#define CRT_VGA_IS0_SENSE (1u << 4)

/* pipe timing register fields */
#define CRT_TIMING_LO(v) ((v) & 0x1fffu)
#define CRT_TIMING_HI(v) (((v) >> 16) & 0x1fffu)

enum crt_dpms {
	CRT_DPMS_ON,
	CRT_DPMS_STANDBY,
	CRT_DPMS_SUSPEND,
	CRT_DPMS_OFF,
};

struct lg_crt {
	uint32_t adpa_reg;
	int has_hotplug; /* the DAC can sense a load on demand */
	int force_hotplug_required; /* PCH: trigger a cycle on the next detect */
};

/* The load-detection mode: 640x480 at 72 Hz. */
static const struct drm_mode_modeinfo crt_load_detect_mode = {
	.clock = 31500,
	.hdisplay = 640,
	.hsync_start = 664,
	.hsync_end = 704,
	.htotal = 832,
	.vdisplay = 480,
	.vsync_start = 489,
	.vsync_end = 492,
	.vtotal = 520,
	.vrefresh = 72,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.type = DRM_MODE_TYPE_DRIVER,
	.name = "640x480",
};

static int crt_is_g45(struct lg_display *d)
{
	return d->is_g4x && !d->is_gm45;
}

static int crt_platform_has_hotplug(struct lg_display *d)
{
	return d->ver >= 4 || d->is_i945g || d->is_i945gm || d->is_g33 || d->is_pnv;
}

/* A detection cycle raises the CRT hotplug interrupt itself; that event is
 * ours, not a monitor coming or going, so it is dropped. */
static void crt_drop_own_hpd_event(struct lg_display *d)
{
	uint32_t bit = 1u << LG_HPD_CRT;

	__atomic_and_fetch(&d->hpd_pending, ~bit, __ATOMIC_SEQ_CST);
	__atomic_and_fetch(&d->hpd_long, ~bit, __ATOMIC_SEQ_CST);
}

/* ---- the DAC ---------------------------------------------------------------- */

static int crt_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	struct lg_crt *c = o->priv;
	uint32_t val = lg_rd(d, c->adpa_reg);

	if (d->pch == LG_PCH_CPT)
		*pipe = (int)((val & ADPA_PIPE_SEL_MASK_CPT) >> CRT_ADPA_PIPE_SEL_SHIFT_CPT);
	else
		*pipe = (int)((val & ADPA_PIPE_SEL_MASK) >> CRT_ADPA_PIPE_SEL_SHIFT);
	return !!(val & ADPA_DAC_ENABLE);
}

static void crt_set_dpms(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg,
			 enum crt_dpms mode)
{
	struct lg_crt *c = o->priv;
	uint32_t adpa = d->ver >= 5 ? CRT_ADPA_HOTPLUG_BITS : 0;

	if (cfg->mode.flags & DRM_MODE_FLAG_PHSYNC)
		adpa |= ADPA_HSYNC_ACTIVE_HIGH;
	if (cfg->mode.flags & DRM_MODE_FLAG_PVSYNC)
		adpa |= ADPA_VSYNC_ACTIVE_HIGH;

	/* Cougar Point can take any of three pipes, the rest A or B */
	if (d->pch == LG_PCH_CPT)
		adpa |= ADPA_PIPE_SEL_CPT(cfg->pipe);
	else
		adpa |= ADPA_PIPE_SEL(cfg->pipe);

	if (d->pch == LG_PCH_NONE)
		lg_wr(d, BCLRPAT(d, cfg->pipe), 0);

	switch (mode) {
	case CRT_DPMS_ON:
		adpa |= ADPA_DAC_ENABLE;
		break;
	case CRT_DPMS_STANDBY:
		adpa |= ADPA_DAC_ENABLE | ADPA_HSYNC_CNTL_DISABLE;
		break;
	case CRT_DPMS_SUSPEND:
		adpa |= ADPA_DAC_ENABLE | ADPA_VSYNC_CNTL_DISABLE;
		break;
	case CRT_DPMS_OFF:
		adpa |= ADPA_HSYNC_CNTL_DISABLE | ADPA_VSYNC_CNTL_DISABLE;
		break;
	}

	lg_wr(d, c->adpa_reg, adpa);
}

static void crt_enable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	crt_set_dpms(d, o, cfg, CRT_DPMS_ON);
}

/* GMCH: off right after the plane. */
static void crt_disable(struct lg_display *d, struct lg_output *o, const struct lg_config *cfg)
{
	crt_set_dpms(d, o, cfg, CRT_DPMS_OFF);
}

/* PCH: the DAC goes off once the pipe and FDI are down. */
static void pch_crt_post_disable(struct lg_display *d, struct lg_output *o,
				 const struct lg_config *cfg)
{
	crt_set_dpms(d, o, cfg, CRT_DPMS_OFF);
}

/* The PCH and Valleyview DAC's hotplug sampling, set up at init and after
 * a resume; the next detect forces a cycle. */
static void crt_reset(struct lg_display *d, struct lg_output *o)
{
	struct lg_crt *c = o->priv;
	uint32_t adpa;

	if (d->ver < 5)
		return;
	adpa = lg_rd(d, c->adpa_reg);
	adpa &= ~CRT_ADPA_HOTPLUG_MASK;
	adpa |= CRT_ADPA_HOTPLUG_BITS;
	lg_wr(d, c->adpa_reg, adpa);
	lg_posting_read(d, c->adpa_reg);
	i915_dbg("[drm] i915: VGA: adpa set to 0x%x\n", adpa);
	c->force_hotplug_required = 1;
}

static void crt_resume(struct lg_display *d, struct lg_output *o)
{
	crt_reset(d, o);
}

static int crt_mode_valid(struct lg_display *d, struct lg_output *o,
			  const struct drm_mode_modeinfo *m)
{
	uint32_t max_clock;

	(void)o;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;
	if (d->ver == 2 && (m->flags & DRM_MODE_FLAG_INTERLACE))
		return -EINVAL;
	if (m->clock < 25000)
		return -EINVAL;

	if (d->is_vlv)
		/* what the PLL can make; the DAC itself is good for 355 MHz */
		max_clock = 270000;
	else if (d->ver == 3 || d->ver == 4)
		max_clock = 400000;
	else
		max_clock = 350000;
	if (m->clock > max_clock)
		return -EINVAL;
	if (d->max_dotclk_khz && m->clock > d->max_dotclk_khz)
		return -EINVAL;
	if (m->hdisplay > 4096)
		return -EINVAL;
	return 0;
}

static int crt_compute_config(struct lg_display *d, struct lg_output *o, struct lg_config *cfg)
{
	(void)o;
	if (cfg->mode.flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;
	if (d->pch != LG_PCH_NONE)
		cfg->has_pch_encoder = 1;
	cfg->port_clock = cfg->mode.clock;
	return 0;
}

/* ---- detection ---------------------------------------------------------------- */

/* PCH: the DAC samples on its own every few seconds; after init or a
 * resume one cycle is forced (with the DAC off) to have a fresh result. */
static int ilk_crt_detect_hotplug(struct lg_display *d, struct lg_output *o)
{
	struct lg_crt *c = o->priv;
	uint32_t adpa;
	int ret;

	if (c->force_hotplug_required) {
		int turn_off_dac = d->pch != LG_PCH_NONE;
		uint32_t save_adpa;

		c->force_hotplug_required = 0;
		save_adpa = adpa = lg_rd(d, c->adpa_reg);
		i915_dbg("[drm] i915: VGA: trigger hotplug detect cycle: adpa=0x%x\n", adpa);

		adpa |= ADPA_CRT_HOTPLUG_FORCE_TRIGGER;
		if (turn_off_dac)
			adpa &= ~ADPA_DAC_ENABLE;
		lg_wr(d, c->adpa_reg, adpa);

		if (lg_wait(d, c->adpa_reg, ADPA_CRT_HOTPLUG_FORCE_TRIGGER, 0, 1000000))
			i915_dbg("[drm] i915: VGA: timed out waiting for FORCE_TRIGGER\n");

		if (turn_off_dac) {
			lg_wr(d, c->adpa_reg, save_adpa);
			lg_posting_read(d, c->adpa_reg);
		}
		crt_drop_own_hpd_event(d);
	}

	/* both blue and green terminated? */
	adpa = lg_rd(d, c->adpa_reg);
	ret = (adpa & ADPA_CRT_HOTPLUG_MONITOR_MASK) != 0;
	i915_dbg("[drm] i915: VGA: ironlake hotplug adpa=0x%x, result %d\n", adpa, ret);
	return ret;
}

/* Valleyview: force a cycle every time.  The cycle raises a hotplug
 * interrupt that would make the hotplug worker probe again and loop, so
 * the event it raises is dropped. */
static int vlv_crt_detect_hotplug(struct lg_display *d, struct lg_output *o)
{
	struct lg_crt *c = o->priv;
	uint32_t adpa, save_adpa;
	int ret;

	save_adpa = adpa = lg_rd(d, c->adpa_reg);
	i915_dbg("[drm] i915: VGA: trigger hotplug detect cycle: adpa=0x%x\n", adpa);

	adpa |= ADPA_CRT_HOTPLUG_FORCE_TRIGGER;
	lg_wr(d, c->adpa_reg, adpa);

	if (lg_wait(d, c->adpa_reg, ADPA_CRT_HOTPLUG_FORCE_TRIGGER, 0, 1000000)) {
		i915_dbg("[drm] i915: VGA: timed out waiting for FORCE_TRIGGER\n");
		lg_wr(d, c->adpa_reg, save_adpa);
	}

	adpa = lg_rd(d, c->adpa_reg);
	ret = (adpa & ADPA_CRT_HOTPLUG_MONITOR_MASK) != 0;
	i915_dbg("[drm] i915: VGA: valleyview hotplug adpa=0x%x, result %d\n", adpa, ret);

	crt_drop_own_hpd_event(d);
	return ret;
}

/* GMCH (945G and later): a forced detection through PORT_HOTPLUG_EN, the
 * result in PORT_HOTPLUG_STAT.  The G45 desktop parts need it twice for a
 * reliable answer. */
static int crt_detect_hotplug(struct lg_display *d, struct lg_output *o)
{
	uint32_t stat;
	int ret = 0, tries;

	if (d->pch != LG_PCH_NONE)
		return ilk_crt_detect_hotplug(d, o);
	if (d->is_vlv)
		return vlv_crt_detect_hotplug(d, o);

	tries = crt_is_g45(d) ? 2 : 1;
	for (int i = 0; i < tries; i++) {
		lg_rmw(d, PORT_HOTPLUG_EN, CRT_HOTPLUG_FORCE_DETECT, CRT_HOTPLUG_FORCE_DETECT);
		if (lg_wait(d, PORT_HOTPLUG_EN, CRT_HOTPLUG_FORCE_DETECT, 0, 1000000))
			i915_dbg("[drm] i915: VGA: timed out waiting for FORCE_DETECT to go off\n");
	}

	stat = lg_rd(d, PORT_HOTPLUG_STAT);
	if ((stat & CRT_HOTPLUG_MONITOR_MASK) != CRT_HOTPLUG_MONITOR_NONE)
		ret = 1;

	/* clear the interrupt the cycle raised */
	lg_wr(d, PORT_HOTPLUG_STAT, CRT_HOTPLUG_INT_STATUS);
	lg_rmw(d, PORT_HOTPLUG_EN, CRT_HOTPLUG_FORCE_DETECT, 0);
	crt_drop_own_hpd_event(d);
	return ret;
}

/* The EDID on the DDC pins; when the GMBUS controller fails, once more
 * bit-banging them. */
static int crt_read_edid(struct lg_display *d, struct lg_output *o)
{
	int rc;

	o->edid_len = 0;
	if (!o->ddc)
		return -ENODEV;
	rc = lg_read_edid(d, o);
	if (rc) {
		i915_dbg("[drm] i915: VGA: GMBUS EDID read failed, retrying with bit-banging\n");
		lg_gmbus_force_bit(d, o->ddc_pin, 1);
		rc = lg_read_edid(d, o);
		lg_gmbus_force_bit(d, o->ddc_pin, 0);
	}
	if (rc == 0 && o->edid_len < 128)
		rc = -ENODEV;
	return rc;
}

/* A DVI-I connector shares its DDC between the analog and the digital
 * side: a digital EDID is not the analog monitor's. */
static int crt_detect_ddc(struct lg_display *d, struct lg_output *o)
{
	if (crt_read_edid(d, o)) {
		i915_dbg("[drm] i915: VGA: not detected via DDC (no valid EDID)\n");
		return 0;
	}
	if (o->edid[20] & 0x80) {
		i915_dbg("[drm] i915: VGA: not detected via DDC (EDID reports a digital panel)\n");
		return 0;
	}
	i915_dbg("[drm] i915: VGA: detected via DDC\n");
	return 1;
}

static int crt_sense_above_threshold(struct lg_display *d)
{
	volatile uint8_t *is0 =
		(volatile uint8_t *)(uintptr_t)(d->i915->mmio_virt + d->base + CRT_VGA_IS0_R);

	return (*is0 & CRT_VGA_IS0_SENSE) != 0;
}

/* The pipe's current scan line. */
static uint32_t crt_dsl(struct lg_display *d, int pipe)
{
	return lg_rd(d, PIPEDSL(d, pipe)) &
	       (d->ver == 2 ? PIPEDSL_LINE_MASK_GEN2 : PIPEDSL_LINE_MASK);
}

/* Load detection with the pipe running for us.  With a border colour
 * (purple) shown instead of the picture, the DAC's comparator says whether
 * the lines are terminated.  Gen3 forces the border over the whole frame;
 * gen2 cannot, so it samples within the vertical border (made if the
 * timing has none) for a whole scan line and calls the monitor present
 * when three quarters of the samples say so. */
static int crt_load_detect(struct lg_display *d, int pipe)
{
	uint32_t save_bclrpat, save_vtotal, vblank;
	uint32_t vtotal, vactive, vblank_start, vblank_end, vsample, dsl;
	int status;

	i915_dbg("[drm] i915: VGA: starting load-detect on pipe %c\n", 'A' + pipe);

	save_bclrpat = lg_rd(d, BCLRPAT(d, pipe));
	save_vtotal = lg_rd(d, TRANS_VTOTAL(d, pipe));
	vblank = lg_rd(d, TRANS_VBLANK(d, pipe));

	vtotal = CRT_TIMING_HI(save_vtotal) + 1;
	vactive = CRT_TIMING_LO(save_vtotal) + 1;
	vblank_start = CRT_TIMING_LO(vblank) + 1;
	vblank_end = CRT_TIMING_HI(vblank) + 1;

	/* purple border */
	lg_wr(d, BCLRPAT(d, pipe), 0x500050);

	if (d->ver != 2) {
		uint32_t conf = lg_rd(d, PIPECONF(d, pipe));

		lg_wr(d, PIPECONF(d, pipe), conf | PIPECONF_FORCE_BORDER);
		lg_posting_read(d, PIPECONF(d, pipe));
		/* the border replaces the picture from the next frame */
		lg_wait_for_vblank(d, pipe);

		status = crt_sense_above_threshold(d);

		lg_wr(d, PIPECONF(d, pipe), conf);
	} else {
		int restore_vblank = 0;
		uint32_t count = 0, detect = 0;
		int n;

		/* no border in the timing: make one (this flickers) */
		if (vblank_start <= vactive && vblank_end >= vtotal) {
			uint32_t vsync = lg_rd(d, TRANS_VSYNC(d, pipe));
			uint32_t vsync_start = CRT_TIMING_LO(vsync) + 1;

			vblank_start = vsync_start;
			lg_wr(d, TRANS_VBLANK(d, pipe),
			      (vblank_start - 1) | ((vblank_end - 1) << 16));
			restore_vblank = 1;
		}
		/* sample in the larger of the two vertical borders */
		if (vblank_start - vactive >= vtotal - vblank_end)
			vsample = (vblank_start + vactive) >> 1;
		else
			vsample = (vtotal + vblank_end) >> 1;

		/* wait for the border: out of it first, then past the sample
		 * line (bounded, should the pipe not run) */
		for (n = 0; n < 100000 && crt_dsl(d, pipe) >= vactive; n++)
			lg_udelay(1);
		dsl = crt_dsl(d, pipe);
		for (n = 0; n < 100000 && dsl <= vsample; n++) {
			lg_udelay(1);
			dsl = crt_dsl(d, pipe);
		}

		/* watch the comparator for a whole scan line */
		do {
			count++;
			if (crt_sense_above_threshold(d))
				detect++;
		} while (crt_dsl(d, pipe) == dsl && count < 1000000);

		if (restore_vblank)
			lg_wr(d, TRANS_VBLANK(d, pipe), vblank);

		status = detect * 4 > count * 3;
	}

	lg_wr(d, BCLRPAT(d, pipe), save_bclrpat);
	return status;
}

/* Hotplug sensing where the DAC has it (trusted only when it says
 * "connected": KVMs often do not pass the sense lines), then the EDID,
 * then -- on the parts without hotplug sensing -- load detection with a
 * pipe lit for it. */
static int crt_detect(struct lg_display *d, struct lg_output *o)
{
	struct lg_crt *c = o->priv;
	int status, pipe;

	o->edid_len = 0;
	if (c->has_hotplug) {
		if (crt_detect_hotplug(d, o)) {
			i915_dbg("[drm] i915: VGA: detected via hotplug\n");
			o->detected = 1;
			return 1;
		}
		i915_dbg("[drm] i915: VGA: not detected via hotplug\n");
	}

	if (crt_detect_ddc(d, o)) {
		o->detected = 1;
		return 1;
	}

	/* load detection is unreliable where hotplug sensing exists */
	if (c->has_hotplug) {
		o->detected = 0;
		return 0;
	}

	pipe = lg_load_detect_get(d, o, &crt_load_detect_mode);
	if (pipe < 0) {
		i915_dbg("[drm] i915: VGA: no pipe for load detection (%d)\n", pipe);
		return o->detected;
	}
	if (crt_detect_ddc(d, o))
		status = 1;
	else if (d->ver < 4)
		status = crt_load_detect(d, pipe);
	else
		status = 0;
	lg_load_detect_release(d, o);

	o->detected = status;
	return status;
}

static uint32_t crt_max_clock(struct lg_display *d)
{
	uint32_t max = d->is_vlv ? 270000 : (d->ver == 3 || d->ver == 4) ? 400000 : 350000;

	if (d->max_dotclk_khz && d->max_dotclk_khz < max)
		max = d->max_dotclk_khz;
	return max;
}

/* The EDID's modes; on G4X a DVI-I connector may carry the analog
 * monitor's EDID on the digital port B pins.  Without an EDID the
 * standard modes up to what the DAC can do. */
static int crt_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	if (o->edid_len < 128 && crt_read_edid(d, o) && (d->is_g4x || d->is_gm45)) {
		struct i2c_adapter *ddc = o->ddc;
		uint8_t pin = o->ddc_pin;

		o->ddc_pin = LG_GMBUS_PIN_DPB;
		o->ddc = lg_gmbus_adapter(d, LG_GMBUS_PIN_DPB);
		if (crt_read_edid(d, o))
			o->edid_len = 0;
		o->ddc = ddc;
		o->ddc_pin = pin;
	}
	return lg_get_modes_edid(d, o, conn, crt_max_clock(d));
}

/* ---- init ---------------------------------------------------------------- */

static const struct lg_output_funcs gmch_crt_funcs = {
	.detect = crt_detect,
	.get_modes = crt_get_modes,
	.mode_valid = crt_mode_valid,
	.compute_config = crt_compute_config,
	.enable = crt_enable,
	.disable = crt_disable,
	.get_hw_state = crt_get_hw_state,
	.resume = crt_resume,
};

static const struct lg_output_funcs pch_crt_funcs = {
	.detect = crt_detect,
	.get_modes = crt_get_modes,
	.mode_valid = crt_mode_valid,
	.compute_config = crt_compute_config,
	.enable = crt_enable,
	.post_disable = pch_crt_post_disable,
	.get_hw_state = crt_get_hw_state,
	.resume = crt_resume,
};

void lg_crt_init(struct lg_display *d)
{
	struct lg_output *o;
	struct lg_crt *c;
	uint32_t adpa_reg, adpa;
	uint8_t pin;

	/* Cherryview has no DAC; Valleyview only when the VBT says so */
	if (d->is_chv)
		return;
	if (d->is_vlv && !d->vbt.int_crt_support) {
		i915_dbg("[drm] i915: VGA: not present per VBT\n");
		return;
	}

	adpa_reg = d->pch != LG_PCH_NONE ? PCH_ADPA : ADPA;
	if (d->pch != LG_PCH_NONE)
		i915_dbg("[drm] i915: VGA: SFUSE_STRAP 0x%x\n", lg_rd(d, SFUSE_STRAP));

	/* Some parts (Ivy Bridge at least) have the DAC fused off with no
	 * fuse bit to say so: the register works but the enable bit does not
	 * stick.  Try it (syncs off) and put the register back. */
	adpa = lg_rd(d, adpa_reg);
	if (!(adpa & ADPA_DAC_ENABLE)) {
		lg_wr(d, adpa_reg,
		      adpa | ADPA_DAC_ENABLE | ADPA_HSYNC_CNTL_DISABLE | ADPA_VSYNC_CNTL_DISABLE);
		if (!(lg_rd(d, adpa_reg) & ADPA_DAC_ENABLE)) {
			i915_dbg("[drm] i915: VGA: DAC enable does not stick, no VGA\n");
			return;
		}
		lg_wr(d, adpa_reg, adpa);
	}

	c = kalloc(sizeof(*c));
	if (!c)
		return;
	mm_memset(c, 0, sizeof(*c));
	c->adpa_reg = adpa_reg;
	c->has_hotplug = crt_platform_has_hotplug(d);

	o = lg_output_new(d);
	if (!o) {
		kfree(c);
		return;
	}

	pin = d->vbt.crt_ddc_pin;
	if (!lg_gmbus_pin_valid(d, pin))
		pin = LG_GMBUS_PIN_VGADDC;

	o->type = LG_OUTPUT_ANALOG;
	o->port = -1;
	o->reg = adpa_reg;
	ksnprintf(o->name, sizeof(o->name), "VGA");
	o->funcs = d->pch != LG_PCH_NONE ? &pch_crt_funcs : &gmch_crt_funcs;
	o->conn_type = DRM_MODE_CONNECTOR_VGA;
	o->enc_type = DRM_MODE_ENCODER_DAC;
	o->pipe_mask = d->is_i830 ? 1u : (1u << d->num_pipes) - 1;
	if (c->has_hotplug) {
		o->hpd_pin = LG_HPD_CRT;
		o->polled = 0;
	} else {
		o->hpd_pin = LG_HPD_NONE;
		o->polled = 1;
	}
	o->ddc_pin = pin;
	o->ddc = lg_gmbus_adapter(d, pin);
	o->priv = c;

	/* GMCH and Valleyview: the comparator threshold and, on G4X, the
	 * longer activation period.  Programming them raises a spurious
	 * hotplug event a few seconds later, so it is done once, here. */
	if (d->pch == LG_PCH_NONE && c->has_hotplug) {
		uint32_t bits = CRT_HOTPLUG_VOLTAGE_COMPARE_50;

		if (d->is_g4x || d->is_gm45)
			bits |= CRT_HOTPLUG_ACTIVATION_PERIOD_64;
		lg_rmw(d, PORT_HOTPLUG_EN,
		       CRT_HOTPLUG_VOLTAGE_COMPARE_MASK | CRT_HOTPLUG_ACTIVATION_PERIOD_64, bits);
	}

	crt_reset(d, o);

	kprintf("[drm] i915: VGA at 0x%x (DDC pin %u, %s)\n", adpa_reg, pin,
		c->has_hotplug ? "hotplug" : "load detection");
}
