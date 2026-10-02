// LikeOS -- the integrated LVDS port of the Intel parts before DDI.
//
// The mobile chipsets from the 855GM to GM45, Pineview, and the PCH of
// Ironlake to Ivy Bridge drive a laptop panel straight from a pipe over
// LVDS: one channel of three or four differential data pairs plus a clock
// (18 or 24 bits a pixel), or two channels for the bigger panels.  The
// port register selects the pipe, powers the pairs, sets the sync
// polarities and (gen4) the dither for 6-bit panels; the panel power
// sequencer switches the panel itself, and a PWM its backlight.  Before
// gen4 only pipe B can drive the port.  A panel shows one timing only:
// its fixed mode comes from its EDID, else from the VBT, else from what
// the firmware left running, and every other mode is scaled onto it by
// the panel fitter.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006-2010 Intel Corporation
// Portions Copyright (C) 2006 Dave Airlie

#include <kernel/dev/gpu/i915/intel_legacy.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

/* The panel power sequencer as the firmware set it up for the panel, put
 * back before every enable (the registers may have been lost since). */
struct lvds_pps {
	uint32_t power_up, backlight_on, backlight_off, power_down; /* 100 us */
	uint32_t power_cycle; /* 100 us */
	uint32_t divider;
	uint32_t port;
	int powerdown_on_reset;
};

struct lvds_priv {
	int is_dual_link;
	uint32_t a3_power; /* LVDS_A3_POWER_*: 24 or 18 bits a pixel */
	uint32_t init_lvds_val; /* the port register as found */
	struct lvds_pps init_pps;
};

static char pipe_name(int pipe)
{
	return pipe >= 0 && pipe < LG_MAX_PIPES ? (char)('A' + pipe) : '?';
}

static uint32_t lvds_pps_base(struct lg_display *d)
{
	return d->gmch ? PPS_BASE_GMCH : PPS_BASE_PCH;
}

static uint32_t mode_vrefresh(const struct drm_mode_modeinfo *m)
{
	uint64_t num, den;

	if (m->vrefresh)
		return m->vrefresh;
	if (!m->htotal || !m->vtotal)
		return 0;
	num = (uint64_t)m->clock * 1000;
	den = (uint64_t)m->htotal * m->vtotal;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	return (uint32_t)((num + den / 2) / den);
}

/* ---- the panel power sequencer (GMCH and PCH, sequencer 0) ------------ */

static void lvds_pps_get_hw_state(struct lg_display *d, struct lvds_pps *pps)
{
	uint32_t base = lvds_pps_base(d);
	uint32_t val;

	pps->powerdown_on_reset = !!(lg_rd(d, PP_CONTROL(base)) & PANEL_POWER_RESET);

	val = lg_rd(d, PP_ON_DELAYS(base));
	pps->port = (val & PANEL_PORT_SELECT_MASK) >> 30;
	pps->power_up = (val & PANEL_POWER_UP_DELAY_MASK) >> PANEL_POWER_UP_DELAY_SHIFT;
	pps->backlight_on = val & PANEL_LIGHT_ON_DELAY_MASK;

	val = lg_rd(d, PP_OFF_DELAYS(base));
	pps->power_down = (val & PANEL_POWER_DOWN_DELAY_MASK) >> PANEL_POWER_DOWN_DELAY_SHIFT;
	pps->backlight_off = val & PANEL_LIGHT_OFF_DELAY_MASK;

	val = lg_rd(d, PP_DIVISOR(base));
	pps->divider = (val & PP_REFERENCE_DIVIDER_MASK) >> PP_REFERENCE_DIVIDER_SHIFT;
	val &= PANEL_POWER_CYCLE_DELAY_MASK;
	/* The register holds the delay plus one 100 ms step (the hardware
	 * documentation adds it because the register takes effect late). */
	if (val)
		val--;
	/* 100 ms to 100 us units */
	pps->power_cycle = val * 1000;

	if (d->ver < 5 && pps->power_up == 0 && pps->backlight_on == 0 &&
	    pps->power_down == 0 && pps->backlight_off == 0) {
		i915_dbg("[drm] i915: LVDS: panel power timings uninitialized, setting defaults\n");
		/* T2 40 ms, T5 200 ms */
		pps->power_up = 40 * 10;
		pps->backlight_on = 200 * 10;
		/* T3 35 ms, Tx 200 ms */
		pps->power_down = 35 * 10;
		pps->backlight_off = 200 * 10;
	}

	i915_dbg("[drm] i915: LVDS PPS: power_up %u power_down %u power_cycle %u backlight_on %u backlight_off %u divider %u port %u powerdown_on_reset %d\n",
		 pps->power_up, pps->power_down, pps->power_cycle, pps->backlight_on,
		 pps->backlight_off, pps->divider, pps->port, pps->powerdown_on_reset);
}

static void lvds_pps_init_hw(struct lg_display *d, const struct lvds_pps *pps)
{
	uint32_t base = lvds_pps_base(d);
	uint32_t val;

	val = lg_rd(d, PP_CONTROL(base));
	if ((val & PANEL_UNLOCK_MASK) != PANEL_UNLOCK_REGS) {
		val &= ~PANEL_UNLOCK_MASK;
		val |= PANEL_UNLOCK_REGS;
	}
	if (pps->powerdown_on_reset)
		val |= PANEL_POWER_RESET;
	lg_wr(d, PP_CONTROL(base), val);

	lg_wr(d, PP_ON_DELAYS(base),
	      ((pps->port << 30) & PANEL_PORT_SELECT_MASK) |
		      ((pps->power_up << PANEL_POWER_UP_DELAY_SHIFT) & PANEL_POWER_UP_DELAY_MASK) |
		      (pps->backlight_on & PANEL_LIGHT_ON_DELAY_MASK));

	lg_wr(d, PP_OFF_DELAYS(base),
	      ((pps->power_down << PANEL_POWER_DOWN_DELAY_SHIFT) & PANEL_POWER_DOWN_DELAY_MASK) |
		      (pps->backlight_off & PANEL_LIGHT_OFF_DELAY_MASK));

	lg_wr(d, PP_DIVISOR(base),
	      ((pps->divider << PP_REFERENCE_DIVIDER_SHIFT) & PP_REFERENCE_DIVIDER_MASK) |
		      ((DIV_ROUND_UP(pps->power_cycle, 1000) + 1) & PANEL_POWER_CYCLE_DELAY_MASK));
}

/* ---- the port ----------------------------------------------------------- */

static int lvds_port_enabled(struct lg_display *d, uint32_t reg, int *pipe)
{
	uint32_t val = lg_rd(d, reg);

	/* the pipe is wanted even when the port is off */
	if (d->pch == LG_PCH_CPT)
		*pipe = (int)((val & LVDS_PIPE_SEL_MASK_CPT) >> 29);
	else
		*pipe = (int)((val & LVDS_PIPE_SEL_MASK) >> 30);
	return !!(val & LVDS_PORT_EN);
}

static int lvds_get_hw_state(struct lg_display *d, struct lg_output *o, int *pipe)
{
	return lvds_port_enabled(d, o->reg, pipe);
}

static int lvds_mode_valid(struct lg_display *d, struct lg_output *o,
			   const struct drm_mode_modeinfo *m)
{
	const struct drm_mode_modeinfo *fixed = &o->fixed_mode;

	if (!o->fixed_mode_valid)
		return -EINVAL;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;
	/* Smaller modes are scaled up onto the panel; nothing larger. */
	if (m->hdisplay > fixed->hdisplay || m->vdisplay > fixed->vdisplay)
		return -EINVAL;
	/* The panel's own size at another refresh rate would not be what
	 * the client asked for (a little latitude for computed modes). */
	if (m->hdisplay == fixed->hdisplay && m->vdisplay == fixed->vdisplay) {
		uint32_t a = mode_vrefresh(m), b = mode_vrefresh(fixed);

		if ((a > b ? a - b : b - a) > 1)
			return -EINVAL;
	}
	/* the pipe always runs the panel's clock */
	if (d->max_dotclk_khz && fixed->clock > d->max_dotclk_khz)
		return -EINVAL;
	return 0;
}

static int lvds_compute_config(struct lg_display *d, struct lg_output *o,
			       struct lg_config *cfg)
{
	struct lvds_priv *p = o->priv;
	int lvds_bpp, ret;

	/* before gen4 only pipe B drives the port */
	if (d->ver < 4 && cfg->pipe == 0) {
		kprintf("[drm] i915: LVDS: cannot drive the panel from pipe A\n");
		return -EINVAL;
	}

	if (!d->gmch)
		cfg->has_pch_encoder = 1;

	/* 24 bits a pixel when the firmware powered the A3 pair, else 18 */
	if (p->a3_power == LVDS_A3_POWER_UP)
		lvds_bpp = 8 * 3;
	else
		lvds_bpp = 6 * 3;
	if (lvds_bpp != cfg->pipe_bpp) {
		i915_dbg("[drm] i915: LVDS: forcing display bpp (was %d) to %d\n",
			 cfg->pipe_bpp, lvds_bpp);
		cfg->pipe_bpp = lvds_bpp;
	}

	if (cfg->mode.flags & DRM_MODE_FLAG_DBLSCAN)
		return -EINVAL;

	/* The pipe runs the panel's timing; the fitter scales the client's
	 * picture (src_w x src_h) onto it. */
	cfg->mode = o->fixed_mode;
	lg_timings_from_mode(&cfg->t, &cfg->mode);
	cfg->t_set = 1;
	cfg->port_clock = o->fixed_mode.clock;

	ret = lg_pfit_compute(d, o, cfg);
	if (ret)
		return ret;
	return 0;
}

/* The EDID's modes (or the fixed mode), plus the standard modes that fit
 * the panel: the fitter shows those. */
static int lvds_get_modes(struct lg_display *d, struct lg_output *o, int conn)
{
	int n = 0, r;
	uint32_t max_clk = d->max_dotclk_khz ? d->max_dotclk_khz : 400000;

	if (o->edid_len > 0) {
		r = drm_connector_set_edid(d->drm, conn, o->edid, (unsigned)o->edid_len);
		if (r > 0)
			n = r;
	}
	if (n <= 0 && o->fixed_mode_valid) {
		if (drm_connector_add_mode(d->drm, conn, &o->fixed_mode) == 0)
			n = 1;
	}
	if (o->fixed_mode_valid) {
		r = drm_connector_add_std_modes(d->drm, conn, max_clk, o->fixed_mode.hdisplay,
						o->fixed_mode.vdisplay);
		if (r > 0)
			n += r;
	}
	return n;
}

/* The port programmed while the pipe and its PLL are still off: the PLL
 * must find the port's clock pairs (and the second channel) powered. */
static void lvds_pre_enable(struct lg_display *d, struct lg_output *o,
			    const struct lg_config *cfg)
{
	struct lvds_priv *p = o->priv;
	int pipe = cfg->pipe;
	uint32_t temp;

	lvds_pps_init_hw(d, &p->init_pps);

	temp = p->init_lvds_val;
	temp |= LVDS_PORT_EN | LVDS_A0A2_CLKA_POWER_UP;

	if (d->pch == LG_PCH_CPT) {
		temp &= ~LVDS_PIPE_SEL_MASK_CPT;
		temp |= LVDS_PIPE_SEL_CPT(pipe);
	} else {
		temp &= ~LVDS_PIPE_SEL_MASK;
		temp |= LVDS_PIPE_SEL(pipe);
	}

	/* the border the fitter's centring needs */
	temp &= ~LVDS_BORDER_ENABLE;
	temp |= cfg->gmch_pfit.lvds_border_bits;

	/* B0-B3 and the second clock for a dual channel panel, matching the
	 * DPLL's dual channel setting */
	if (p->is_dual_link)
		temp |= LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP;
	else
		temp &= ~(LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP);

	/* 24 vs 18 bit: what the firmware chose, kept */
	temp &= ~LVDS_A3_POWER_MASK;
	temp |= p->a3_power;

	/* Gen4 has a dither bit in the port, meant for 18 bpp panels only;
	 * the PCH parts dither in the transcoder, gen2/3 in the fitter. */
	if (d->ver == 4) {
		if (cfg->dither && cfg->pipe_bpp == 18)
			temp |= LVDS_ENABLE_DITHER;
		else
			temp &= ~LVDS_ENABLE_DITHER;
	}

	temp &= ~(LVDS_HSYNC_POLARITY | LVDS_VSYNC_POLARITY);
	if (cfg->mode.flags & DRM_MODE_FLAG_NHSYNC)
		temp |= LVDS_HSYNC_POLARITY;
	if (cfg->mode.flags & DRM_MODE_FLAG_NVSYNC)
		temp |= LVDS_VSYNC_POLARITY;

	i915_dbg("[drm] i915: LVDS: port %08x on pipe %c\n", temp, pipe_name(pipe));
	lg_wr(d, o->reg, temp);
}

/* Port on, then the panel's power through the sequencer, then the
 * backlight. */
static void lvds_enable(struct lg_display *d, struct lg_output *o,
			const struct lg_config *cfg)
{
	uint32_t base = lvds_pps_base(d);

	lg_rmw(d, o->reg, 0, LVDS_PORT_EN);

	lg_rmw(d, PP_CONTROL(base), 0, PANEL_POWER_ON);
	lg_posting_read(d, o->reg);

	if (lg_wait(d, PP_STATUS(base), PP_ON, PP_ON, 5000u * 1000u))
		kprintf("[drm] i915: LVDS: timed out waiting for the panel to power on (PP_STATUS %08x)\n",
			lg_rd(d, PP_STATUS(base)));

	lg_backlight_enable(d, o, cfg);
}

static void lvds_port_disable(struct lg_display *d, struct lg_output *o)
{
	uint32_t base = lvds_pps_base(d);

	lg_rmw(d, PP_CONTROL(base), PANEL_POWER_ON, 0);
	if (lg_wait(d, PP_STATUS(base), PP_ON, 0, 1000u * 1000u))
		kprintf("[drm] i915: LVDS: timed out waiting for the panel to power off (PP_STATUS %08x)\n",
			lg_rd(d, PP_STATUS(base)));

	lg_rmw(d, o->reg, LVDS_PORT_EN, 0);
	lg_posting_read(d, o->reg);
}

/* GMCH: everything before the pipe goes off. */
static void gmch_disable_lvds(struct lg_display *d, struct lg_output *o,
			      const struct lg_config *cfg)
{
	(void)cfg;
	lg_backlight_disable(d, o);
	lvds_port_disable(d, o);
}

/* PCH: the backlight before the pipe goes off, the panel and port only
 * after the CPU pipe and FDI are down. */
static void pch_disable_lvds(struct lg_display *d, struct lg_output *o,
			     const struct lg_config *cfg)
{
	(void)cfg;
	lg_backlight_disable(d, o);
}

static void pch_post_disable_lvds(struct lg_display *d, struct lg_output *o,
				  const struct lg_config *cfg)
{
	(void)cfg;
	lvds_port_disable(d, o);
}

static const struct lg_output_funcs lvds_funcs_gmch = {
	.get_modes = lvds_get_modes,
	.mode_valid = lvds_mode_valid,
	.compute_config = lvds_compute_config,
	.pre_enable = lvds_pre_enable,
	.enable = lvds_enable,
	.disable = gmch_disable_lvds,
	.get_hw_state = lvds_get_hw_state,
};

static const struct lg_output_funcs lvds_funcs_pch = {
	.get_modes = lvds_get_modes,
	.mode_valid = lvds_mode_valid,
	.compute_config = lvds_compute_config,
	.pre_enable = lvds_pre_enable,
	.enable = lvds_enable,
	.disable = pch_disable_lvds,
	.post_disable = pch_post_disable_lvds,
	.get_hw_state = lvds_get_hw_state,
};

/* ---- discovery ------------------------------------------------------------ */

/* Is there an LVDS port at all?  The PCH has a strap for it; before,
 * LVDS came only with the mobile parts (not the 830M). */
static int lvds_supported(struct lg_display *d)
{
	if (d->pch == LG_PCH_IBX || d->pch == LG_PCH_CPT)
		return !!(lg_rd(d, PCH_LVDS) & LVDS_DETECTED);
	if (!d->gmch || d->is_vlv || d->is_chv)
		return 0;
	if (d->is_pnv)
		return 1;
	if (d->ver == 3 || d->ver == 4)
		return d->mobile;
	if (d->ver == 2)
		return d->is_i85x;
	return 0;
}

/* The firmware's SSC choice for the panel clock, and its frequency. */
static void lvds_init_ssc(struct lg_display *d, uint32_t lvds)
{
	d->lvds_use_ssc = d->vbt.lvds_use_ssc;
	d->lvds_ssc_khz = d->vbt.lvds_ssc_freq_khz;

	if (!d->vbt.valid || !d->lvds_ssc_khz) {
		/* the VBT's defaults: Ironlake to Ivy Bridge take a 120 MHz
		 * spread clock, the GMCH parts their alternative SSC input */
		if (!d->gmch)
			d->lvds_ssc_khz = 120000;
		else if (d->ver == 2)
			d->lvds_ssc_khz = 66667;
		else
			d->lvds_ssc_khz = 100000;
	}

	if (d->pch == LG_PCH_IBX || d->pch == LG_PCH_CPT) {
		/* There may be no VBT; and when the BIOS enabled SSC, keeping
		 * it avoids a flicker, while when it did not, SSC may not work
		 * even if the VBT says so. */
		int bios_ssc = !!(lg_rd(d, PCH_DREF_CONTROL) & DREF_SSC1_ENABLE);

		if (bios_ssc != !!d->lvds_use_ssc) {
			i915_dbg("[drm] i915: SSC %s by BIOS, overriding VBT which says %s\n",
				 bios_ssc ? "enabled" : "disabled",
				 d->lvds_use_ssc ? "enabled" : "disabled");
			d->lvds_use_ssc = bios_ssc;
		}
	} else if (!d->vbt.valid && (lvds & LVDS_PORT_EN)) {
		/* no VBT: what the running panel's PLL uses */
		int pipe = (int)((lvds & LVDS_PIPE_SEL_MASK) >> 30);
		uint32_t dpll = lg_rd(d, DPLL(d, pipe));

		if (dpll & DPLL_VCO_ENABLE)
			d->lvds_use_ssc = (dpll & PLL_REF_INPUT_MASK) ==
					  PLLB_REF_INPUT_SPREADSPECTRUMIN;
	}
}

static int compute_is_dual_link_lvds(struct lg_display *d, uint32_t reg,
				     const struct drm_mode_modeinfo *fixed)
{
	uint32_t val;

	/* a single channel carries at most 112 MHz */
	if (fixed->clock > 112999)
		return 1;

	/* The BIOS should have set the port at boot, but leaves it alone
	 * with the lid closed: then the VBT's "value to be set" tells. */
	val = lg_rd(d, reg);
	if (d->pch == LG_PCH_CPT)
		val &= ~(LVDS_DETECTED | LVDS_PIPE_SEL_MASK_CPT);
	else
		val &= ~(LVDS_DETECTED | LVDS_PIPE_SEL_MASK);
	if (val == 0)
		val = d->vbt.bios_lvds_val;

	return (val & LVDS_CLKB_POWER_MASK) == LVDS_CLKB_POWER_UP;
}

/* The panel's EDID over its DDC pin: the preferred mode (else the first)
 * and the physical size.  0, or -ENODEV. */
static int lvds_probe_edid(struct lg_display *d, struct lg_output *probe,
			   struct drm_mode_modeinfo *mode)
{
	struct drm_edid_info *info;
	int ret = -ENODEV;

	if (!probe->ddc || lg_read_edid(d, probe) != 0 || probe->edid_len <= 0)
		goto none;

	info = kalloc(sizeof(*info));
	if (!info)
		goto none;
	mm_memset(info, 0, sizeof(*info));
	if (drm_edid_parse(probe->edid, (unsigned)probe->edid_len, info) == 0 &&
	    info->nmodes > 0) {
		int i = (info->preferred >= 0 && info->preferred < info->nmodes) ?
				info->preferred : 0;

		*mode = info->modes[i];
		probe->mm_width = info->mm_width;
		probe->mm_height = info->mm_height;
		ret = 0;
	}
	kfree(info);
	if (ret == 0)
		return 0;
none:
	/* not an EDID one can use: forget it */
	probe->edid_len = 0;
	return -ENODEV;
}

void lg_lvds_init(struct lg_display *d)
{
	struct lg_output *probe = NULL, *o;
	struct lvds_priv *p = NULL;
	struct drm_mode_modeinfo fixed;
	const char *src = NULL;
	uint32_t reg, lvds;
	uint8_t pin;
	int pipe;

	d->lvds_dual = 0;
	reg = d->gmch ? LVDS : PCH_LVDS;
	lvds = (d->is_vlv || d->is_chv) ? 0 : lg_rd(d, reg);
	lvds_init_ssc(d, lvds);

	if (!lvds_supported(d))
		return;

	if (!d->vbt.int_lvds_support) {
		i915_dbg("[drm] i915: internal LVDS support disabled by VBT\n");
		return;
	}

	if (!d->gmch && !(lvds & LVDS_DETECTED))
		return;

	pin = LG_GMBUS_PIN_PANEL;
	if (!lg_vbt_lvds_present(d, &pin)) {
		if (!(lvds & LVDS_PORT_EN)) {
			i915_dbg("[drm] i915: LVDS is not present in VBT\n");
			return;
		}
		i915_dbg("[drm] i915: LVDS is not present in VBT, but enabled anyway\n");
	}

	p = kalloc(sizeof(*p));
	probe = kalloc(sizeof(*probe));
	if (!p || !probe)
		goto fail;
	mm_memset(p, 0, sizeof(*p));
	mm_memset(probe, 0, sizeof(*probe));
	probe->type = LG_OUTPUT_LVDS;
	probe->reg = reg;
	probe->conn = -1;
	probe->crtc = -1;
	probe->pipe = -1;
	probe->sibling = -1;
	probe->is_panel = 1;
	ksnprintf(probe->name, sizeof(probe->name), "LVDS");

	lvds_pps_get_hw_state(d, &p->init_pps);
	p->init_lvds_val = lvds;

	/* the panel's DDC: the VBT's pin, else the panel pin */
	if (!lg_gmbus_pin_valid(d, pin))
		pin = LG_GMBUS_PIN_PANEL;
	probe->ddc_pin = pin;
	probe->ddc = lg_gmbus_adapter(d, pin);
	if (!probe->ddc && pin != LG_GMBUS_PIN_PANEL) {
		probe->ddc_pin = LG_GMBUS_PIN_PANEL;
		probe->ddc = lg_gmbus_adapter(d, LG_GMBUS_PIN_PANEL);
	}

	/* The fixed mode: the EDID on the panel's DDC, else the VBT's
	 * panel timing, else what the firmware runs on the port's pipe. */
	mm_memset(&fixed, 0, sizeof(fixed));
	if (lvds_probe_edid(d, probe, &fixed) == 0) {
		src = "EDID";
	} else if (d->vbt.lfp_mode_valid) {
		fixed = d->vbt.lfp_mode;
		probe->mm_width = d->vbt.lfp_width_mm;
		probe->mm_height = d->vbt.lfp_height_mm;
		src = "VBT";
	} else if (lvds_port_enabled(d, reg, &pipe) &&
		   lg_pipe_read_mode(d, pipe, &fixed) == 0) {
		src = "current (BIOS)";
	}
	if (!src || !fixed.hdisplay || !fixed.vdisplay || !fixed.clock) {
		i915_dbg("[drm] i915: no LVDS modes found, disabling\n");
		goto fail;
	}
	fixed.type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
	if (!fixed.vrefresh || !fixed.name[0])
		drm_mode_finish(&fixed);
	if (!probe->mm_width || !probe->mm_height) {
		probe->mm_width = d->vbt.lfp_width_mm;
		probe->mm_height = d->vbt.lfp_height_mm;
	}

	o = lg_output_new(d);
	if (!o) {
		kprintf("[drm] i915: LVDS: no free output slot\n");
		goto fail;
	}
	o->type = LG_OUTPUT_LVDS;
	o->port = -1; /* LVDS has no port letter */
	o->reg = reg;
	ksnprintf(o->name, sizeof(o->name), "LVDS");
	o->funcs = d->gmch ? &lvds_funcs_gmch : &lvds_funcs_pch;
	o->conn_type = DRM_MODE_CONNECTOR_LVDS;
	o->enc_type = DRM_MODE_ENCODER_LVDS;
	o->pipe_mask = d->ver < 4 ? (1u << 1) : (1u << d->num_pipes) - 1;
	o->hpd_pin = LG_HPD_NONE;
	o->polled = 0; /* a panel is always there */
	o->ddc_pin = probe->ddc_pin;
	o->ddc = probe->ddc;
	if (probe->edid_len > 0 && probe->edid_len <= (int)sizeof(o->edid)) {
		mm_memcpy(o->edid, probe->edid, (size_t)probe->edid_len);
		o->edid_len = probe->edid_len;
	}
	o->detected = 1;
	o->is_panel = 1;
	o->fixed_mode = fixed;
	o->fixed_mode_valid = 1;
	o->mm_width = probe->mm_width;
	o->mm_height = probe->mm_height;
	o->priv = p;
	kfree(probe);
	probe = NULL;

	lg_backlight_setup(d, o, -1);

	p->is_dual_link = compute_is_dual_link_lvds(d, reg, &fixed);
	p->a3_power = lvds & LVDS_A3_POWER_MASK;
	d->lvds_dual = p->is_dual_link;

	kprintf("[drm] i915: LVDS panel %ux%u@%u (%s fixed mode, %u kHz), %s-link, %d bpp, %ux%u mm, SSC %s\n",
		fixed.hdisplay, fixed.vdisplay, mode_vrefresh(&fixed), src, fixed.clock,
		p->is_dual_link ? "dual" : "single",
		p->a3_power == LVDS_A3_POWER_UP ? 24 : 18, o->mm_width, o->mm_height,
		d->lvds_use_ssc ? "on" : "off");
	return;

fail:
	if (probe)
		kfree(probe);
	if (p)
		kfree(p);
}
