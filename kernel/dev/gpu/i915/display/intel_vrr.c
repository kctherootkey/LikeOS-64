// LikeOS -- the variable refresh timing generator of the Intel display
// transcoders.
//
// From display version 30 even a fixed refresh rate runs through the
// variable-refresh timing generator, with its minimum, maximum and flip
// line all at the vertical total: it is switched on after the
// transcoder's DDI function and before the transcoder, and off after the
// transcoder has stopped.
//
// Variable refresh proper (display version 11 and later, DisplayPort and
// embedded panels): a sink that can ignore the MSA timing parameters,
// is not behind a branch device and has a refresh range of more than 10
// Hz in its EDID (an embedded panel also needs the VBT's permission) is
// "capable"; the crtc's VRR_ENABLED asks for the range.  The frame then
// lasts from the mode's vertical total (the minimum and the flip line:
// a flip that arrives sooner waits for it) up to the line count of the
// range's lowest refresh (the maximum).  The guard band -- the lines
// before the end of a frame at which the vblank starts and the
// double-buffered registers latch -- is the whole vblank, kept the same
// for the fixed and the variable timing, so switching between them
// never moves the vblank.  The sink is told to ignore the MSA timing
// whenever its range holds the mode's refresh, from the link training
// on; the generator's range is set up with the transcoder and switched
// on once the pipe runs; every flip sends a push that ends the frame as
// soon as the minimum allows.
//
// Display 11 and 12 count the guard band as "pipeline full" lines, lose
// a line to an extra vblank delay and need the flip line above the
// minimum, so the minimum is programmed one line short.  Display 30 has
// the generator running all the time (the fixed-rate path above), so
// variable refresh only changes its range there.
//
// I915_FEAT_VRR 0: only the display 30 fixed-rate use, as before.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2020 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/i915/intel_vrr.h>
#include <kernel/dev/gpu/i915/intel_vrr_regs.h>
#include <kernel/dev/gpu/drm_dp_helper.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>

/* The variable-refresh timing generator's guard band: the whole vblank
 * less the set context latency (the longest the hardware allows). */
static uint32_t vrr_guardband(struct i915_device *i915, const struct drm_mode_modeinfo *m)
{
	uint32_t gb = m->vtotal - m->vdisplay - intel_set_context_latency(i915);
	return gb > 0xffff ? 0xffff : gb;
}

/* Display 30+: after the DDI function, before the transcoder. */
static void vrr_tg_enable(struct i915_device *i915, struct intel_pipe *p)
{
	int t = p->transcoder;
	const struct drm_mode_modeinfo *m = &p->mode;
	if (i915->info->display_ver < 30)
		return;
	i915_write32(i915, TRANS_VRR_VMIN(t), m->vtotal - 1);
	i915_write32(i915, TRANS_VRR_VMAX(t), m->vtotal - 1);
	i915_write32(i915, TRANS_VRR_FLIPLINE(t), m->vtotal - 1);
	i915_write32(i915, TRANS_PUSH(t), TRANS_PUSH_EN);
	i915_write32(i915, TRANS_VRR_CTL(t), VRR_CTL_VRR_ENABLE | VRR_CTL_FLIP_LINE_EN |
						     XELPD_VRR_CTL_VRR_GUARDBAND(vrr_guardband(i915, m)));
}

/* And after the transcoder has stopped, before its DDI function goes. */
static void vrr_tg_disable(struct i915_device *i915, struct intel_pipe *p)
{
	int t = p->transcoder;
	if (i915->info->display_ver < 30)
		return;
	i915_write32(i915, TRANS_VRR_CTL(t), VRR_CTL_FLIP_LINE_EN |
						     XELPD_VRR_CTL_VRR_GUARDBAND(vrr_guardband(i915, &p->mode)));
	for (int w = 0; w < 1000; w++) {
		if (!(i915_read32(i915, TRANS_VRR_STATUS(t)) & VRR_STATUS_VRR_EN_LIVE))
			break;
		lapic_delay_us(1000);
	}
	i915_write32(i915, TRANS_PUSH(t), i915_read32(i915, TRANS_PUSH(t)) & ~TRANS_PUSH_EN);
}

/* ---- the timing: what a mode and a sink's range make of the generator ------- */

/* The frame start delay the transcoders run with: the field is left at
 * its reset value, one line. */
#define VRR_FRAMESTART_DELAY 1

/* What the generator is to run for a mode on an output.  All zero: the
 * generator is not used for variable refresh on this pipe. */
struct vrr_config {
	int in_range; /* the sink's range holds the mode's refresh */
	int enable; /* variable refresh runs (else vmin == vmax == flipline) */
	int vmin, vmax, flipline; /* frame lengths in lines */
	int guardband, pipeline_full;
};

/* Display 11/12 insert one extra line just after the active area, which
 * moves the minimum's decision point down a line and so shortens the
 * longest guard band by one. */
static int vrr_extra_vblank_delay(int ver)
{
	return ver < 13 ? 1 : 0;
}

/* Display 11/12 need the flip line at least one above the minimum: the
 * minimum is programmed a line short, the flip line keeps the frame's
 * real minimum length. */
static int vrr_vmin_flipline_offset(int ver)
{
	return ver < 13 ? 1 : 0;
}

/* The hardware adds one line of its own between the two counts. */
static int vrr_guardband_to_pipeline_full(int guardband)
{
	return guardband - VRR_FRAMESTART_DELAY - 1;
}

static int vrr_pipeline_full_to_guardband(int pipeline_full)
{
	return pipeline_full + VRR_FRAMESTART_DELAY + 1;
}

/* The mode's refresh in Hz, rounded to the nearest. */
static int vrr_mode_vrefresh(const struct drm_mode_modeinfo *m)
{
	uint64_t num = (uint64_t)m->clock * 1000;
	uint64_t den = (uint64_t)m->htotal * m->vtotal;

	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		num *= 2;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		den *= 2;
	if (m->vscan > 1)
		den *= m->vscan;
	if (!den)
		return 0;
	return (int)((num + den / 2) / den);
}

/* The longest frame: the lines of the range's lowest refresh at the
 * mode's pixel clock and line length, never shorter than the mode's own
 * frame (and no longer than the maximum's register holds). */
static int vrr_compute_vmax(const struct drm_mode_modeinfo *m, int min_vfreq)
{
	uint64_t den = (uint64_t)m->htotal * (uint32_t)min_vfreq;
	uint64_t vmax = den ? (uint64_t)m->clock * 1000 / den : 0;

	if (vmax < m->vtotal)
		vmax = m->vtotal;
	if (vmax > (uint64_t)VRR_VMAX_MASK + 1)
		vmax = (uint64_t)VRR_VMAX_MASK + 1;
	return (int)vmax;
}

/* The generator's timing for mode `m' on display version `ver' with set
 * context latency `scl': `in_range' the sink's range [min_vfreq, ..]
 * holds the mode's refresh, `vrr_enabled' the client asked for variable
 * refresh.  Without a range (or a vblank too short for the generator)
 * everything stays zero. */
static void vrr_compute_timings(int ver, int scl, const struct drm_mode_modeinfo *m, int in_range,
				int min_vfreq, int vrr_enabled, struct vrr_config *c)
{
	int vmin, vmax, guardband, max_hw, max_vblank;

	*c = (struct vrr_config){ 0 };
	if (!in_range || !m->vtotal || !m->htotal || !m->clock || m->vtotal <= m->vdisplay)
		return;
	c->in_range = 1;
	/* The minimum is the mode's own frame, for the fixed and the
	 * variable timing alike: the guard band then never changes between
	 * the two. */
	vmin = m->vtotal;
	vmax = vrr_compute_vmax(m, min_vfreq);
	if (vrr_enabled && vmin < vmax) {
		c->vmin = vmin;
		c->vmax = vmax;
		c->flipline = vmin;
		c->enable = 1;
	} else {
		c->vmin = c->vmax = c->flipline = m->vtotal;
	}

	/* The guard band: the whole vblank, as long as the register (or the
	 * pipeline-full count) and the vblank less the set context latency
	 * allow. */
	guardband = c->vmin - m->vdisplay;
	if (ver >= 13)
		max_hw = XELPD_VRR_CTL_VRR_GUARDBAND_MAX;
	else
		max_hw = vrr_pipeline_full_to_guardband(VRR_CTL_PIPELINE_FULL_MAX);
	max_vblank = c->vmin - m->vdisplay - scl - vrr_extra_vblank_delay(ver);
	guardband = min(guardband, min(max_hw, max_vblank));
	c->guardband = guardband;
	if (ver < 13)
		c->pipeline_full = vrr_guardband_to_pipeline_full(guardband);
	if (guardband < 1 || c->pipeline_full < 0)
		*c = (struct vrr_config){ 0 };
}

/* What the generator's control word holds besides the enable bit. */
static uint32_t vrr_trans_ctl(int ver, const struct vrr_config *c)
{
	if (ver >= 14)
		return VRR_CTL_FLIP_LINE_EN | XELPD_VRR_CTL_VRR_GUARDBAND(c->guardband);
	if (ver >= 13)
		return VRR_CTL_IGN_MAX_SHIFT | VRR_CTL_FLIP_LINE_EN |
		       XELPD_VRR_CTL_VRR_GUARDBAND(c->guardband);
	return VRR_CTL_IGN_MAX_SHIFT | VRR_CTL_FLIP_LINE_EN | VRR_CTL_PIPELINE_FULL(c->pipeline_full) |
	       VRR_CTL_PIPELINE_FULL_OVERRIDE;
}

/* A line count as the registers take it: on display 11/12 less the set
 * context latency (the frame keeps its length). */
static int vrr_hw_value(int ver, int scl, int value)
{
	return ver >= 13 ? value : value - scl;
}

/* ---- the device side ------------------------------------------------------------ */

static int vrr_supported(struct i915_device *i915)
{
	return I915_FEAT_VRR && i915->info->display_ver >= 11 && !intel_display_legacy_part(i915);
}

static int vrr_output_is_dp(const struct intel_output *o)
{
	return o && (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP);
}

static void vrr_barrier(void)
{
	__asm__ volatile("" ::: "memory");
}

/* Can the sink on `o' run a variable refresh, from its DPCD and the
 * refresh range of the EDID just parsed into `info'? */
static int vrr_is_capable(struct i915_device *i915, const struct intel_output *o,
			  const struct drm_display_info *info)
{
	if (!vrr_supported(i915) || !vrr_output_is_dp(o))
		return 0;
	/* an embedded panel only where the VBT allows it */
	if ((o->type == INTEL_OUTPUT_EDP || o->is_edp) && !i915->display.vbt.lfp_vrr)
		return 0;
	/* A protocol converter in front of the sink (an HDMI 2.1 sink
	 * behind a PCON could, but that is not driven here). */
	if (drm_dp_is_branch(o->dpcd))
		return 0;
	/* the sink must be able to ignore the MSA timing */
	if (!drm_dp_sink_can_do_video_without_timing_msa(o->dpcd))
		return 0;
	if (!info->monitor_range.min_vfreq || !info->monitor_range.max_vfreq ||
	    info->monitor_range.min_vfreq > info->monitor_range.max_vfreq)
		return 0;
	/* anything narrower is no use */
	return info->monitor_range.max_vfreq - info->monitor_range.min_vfreq > 10;
}

/* The generator's timing for mode `m' on output `o'. */
static void vrr_compute(struct i915_device *i915, const struct intel_output *o,
			const struct drm_mode_modeinfo *m, int vrr_enabled, struct vrr_config *c)
{
	int refresh, in_range;

	*c = (struct vrr_config){ 0 };
	if (!vrr_supported(i915) || !vrr_output_is_dp(o) || !o->vrr.capable)
		return;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		return;
	refresh = vrr_mode_vrefresh(m);
	in_range = refresh >= o->vrr.min_vrefresh && refresh <= o->vrr.max_vrefresh;
	vrr_compute_timings(i915->info->display_ver, (int)intel_set_context_latency(i915), m,
			    in_range, o->vrr.min_vrefresh, vrr_enabled, c);
}

static int vrr_same_timing(const struct intel_vrr_pipe_state *s, const struct vrr_config *c)
{
	return s->vmin == (uint32_t)c->vmin && s->vmax == (uint32_t)c->vmax &&
	       s->flipline == (uint32_t)c->flipline && s->guardband == (uint32_t)c->guardband &&
	       s->pipeline_full == (uint32_t)c->pipeline_full;
}

/* The pipe's record of the generator (not `enabled'). */
static void vrr_store(struct intel_pipe *p, const struct vrr_config *c)
{
	p->vrr.in_range = (uint8_t)c->in_range;
	p->vrr.vmin = (uint32_t)c->vmin;
	p->vrr.vmax = (uint32_t)c->vmax;
	p->vrr.flipline = (uint32_t)c->flipline;
	p->vrr.guardband = (uint32_t)c->guardband;
	p->vrr.pipeline_full = (uint32_t)c->pipeline_full;
}

/* The range registers, from frame lengths. */
static void vrr_write_range(struct i915_device *i915, int t, int vmin, int vmax, int flipline)
{
	int ver = i915->info->display_ver;
	int scl = (int)intel_set_context_latency(i915);

	i915_write32(i915, TRANS_VRR_VMIN(t),
		     (uint32_t)(vrr_hw_value(ver, scl, vmin) - vrr_vmin_flipline_offset(ver) - 1));
	i915_write32(i915, TRANS_VRR_VMAX(t), (uint32_t)(vrr_hw_value(ver, scl, vmax) - 1));
	i915_write32(i915, TRANS_VRR_FLIPLINE(t), (uint32_t)(vrr_hw_value(ver, scl, flipline) - 1));
}

/* The fixed timing (the mode's vertical total everywhere). */
static void vrr_write_fixed_range(struct i915_device *i915, int t, const struct drm_mode_modeinfo *m)
{
	vrr_write_range(i915, t, m->vtotal, m->vtotal, m->vtotal);
}

/* Display 11..29: the generator's fixed timing and guard band, set up
 * (not switched on) after the DDI function and before the transcoder --
 * the DDI function must be on first, or Ice Lake can hang.  Also used on
 * a running pipe whose transcoder was set up without a range. */
static void vrr_set_transcoder_timings(struct i915_device *i915, struct intel_pipe *p,
				       const struct vrr_config *c)
{
	int ver = i915->info->display_ver;
	int t = p->transcoder;
	static int warned;

	if (!(i915_read32(i915, TRANS_DDI_FUNC_CTL(t)) & TRANS_DDI_FUNC_ENABLE)) {
		if (!warned++)
			kprintf("[drm] i915: transcoder %d: variable refresh set up before its DDI function, skipped\n",
				t);
		return;
	}
	if (ver == 12 || ver == 13)
		i915_write32(i915, VRR_CHICKEN_TRANS(t),
			     i915_read32(i915, VRR_CHICKEN_TRANS(t)) | VRR_PIPE_VBLANK_WITH_DELAY);
	vrr_write_fixed_range(i915, t, &p->mode);
	i915_write32(i915, TRANS_VRR_CTL(t), vrr_trans_ctl(ver, c));
}

/* Variable refresh on: the range, then (where the generator does not run
 * all the time) the push and the generator itself. */
static void vrr_hw_enable(struct i915_device *i915, struct intel_pipe *p, const struct vrr_config *c)
{
	int ver = i915->info->display_ver;
	int t = p->transcoder;

	vrr_write_range(i915, t, c->vmin, c->vmax, c->flipline);
	if (ver >= 30)
		return;
	i915_write32(i915, TRANS_PUSH(t), TRANS_PUSH_EN);
	i915_write32(i915, TRANS_VRR_CTL(t), VRR_CTL_VRR_ENABLE | vrr_trans_ctl(ver, c));
}

/* Variable refresh off: the generator stopped (where it does not run all
 * the time) and the range back at the fixed timing. */
static void vrr_hw_disable(struct i915_device *i915, struct intel_pipe *p, const struct vrr_config *c)
{
	int ver = i915->info->display_ver;
	int t = p->transcoder;
	static int warned;

	if (ver < 30) {
		int w;

		i915_write32(i915, TRANS_VRR_CTL(t), vrr_trans_ctl(ver, c));
		for (w = 0; w < 1000; w++) {
			if (!(i915_read32(i915, TRANS_VRR_STATUS(t)) & VRR_STATUS_VRR_EN_LIVE))
				break;
			lapic_delay_us(1000);
		}
		if (w == 1000 && !warned++)
			kprintf("[drm] i915: transcoder %d: variable refresh did not stop\n", t);
		i915_write32(i915, TRANS_PUSH(t), i915_read32(i915, TRANS_PUSH(t)) & ~TRANS_PUSH_EN);
	}
	vrr_write_fixed_range(i915, t, &p->mode);
}

/* The record of what the generator runs, from the pipe (`enabled' aside). */
static void vrr_config_of_pipe(const struct intel_pipe *p, struct vrr_config *c)
{
	*c = (struct vrr_config){ 0 };
	c->in_range = p->vrr.in_range;
	c->enable = p->vrr.enabled;
	c->vmin = (int)p->vrr.vmin;
	c->vmax = (int)p->vrr.vmax;
	c->flipline = (int)p->vrr.flipline;
	c->guardband = (int)p->vrr.guardband;
	c->pipeline_full = (int)p->vrr.pipeline_full;
}

/* Switch variable refresh off on pipe `p' (it runs). */
static void vrr_pipe_off(struct i915_device *i915, struct intel_pipe *p)
{
	struct vrr_config c;

	vrr_config_of_pipe(p, &c);
	/* the vblank code stops using the range first */
	p->vrr.enabled = 0;
	vrr_barrier();
	vrr_hw_disable(i915, p, &c);
	p->vrr.vmin = p->vrr.vmax = p->vrr.flipline = p->mode.vtotal;
}

/* ---- the calls ------------------------------------------------------------------ */

void intel_vrr_transcoder_enable(struct i915_device *i915, struct intel_pipe *p)
{
	struct intel_output *o = NULL;
	struct vrr_config c;

	vrr_tg_enable(i915, p);
	if (!vrr_supported(i915))
		return;
	p->vrr = (struct intel_vrr_pipe_state){ 0 };
	if (p->output >= 0 && p->output < i915->display.nout)
		o = &i915->display.outputs[p->output];
	if (!vrr_output_is_dp(o))
		return;
	vrr_compute(i915, o, &p->mode, 0, &c);
	if (!c.in_range)
		return;
	vrr_store(p, &c);
	if (i915->info->display_ver < 30)
		vrr_set_transcoder_timings(i915, p, &c);
}

void intel_vrr_transcoder_disable(struct i915_device *i915, struct intel_pipe *p)
{
	vrr_tg_disable(i915, p);
	if (!vrr_supported(i915))
		return;
	if (i915->info->display_ver < 30 && (p->vrr.in_range || p->vrr.enabled)) {
		int t = p->transcoder;

		/* the transcoder has stopped: leave the generator as a
		 * transcoder that never had a range finds it */
		p->vrr.enabled = 0;
		vrr_barrier();
		i915_write32(i915, TRANS_VRR_CTL(t), 0);
		i915_write32(i915, TRANS_PUSH(t), i915_read32(i915, TRANS_PUSH(t)) & ~TRANS_PUSH_EN);
	}
	p->vrr.enabled = 0;
	vrr_barrier();
	p->vrr = (struct intel_vrr_pipe_state){ 0 };
}

void intel_vrr_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	if (!vrr_supported(i915))
		return;
	drv->features |= DRM_FEATURE_VRR;
}

void intel_vrr_connector_init(struct i915_device *i915, struct intel_output *o, int conn)
{
	struct drm_device *dev = &i915->drm;
	int rc;

	if (!vrr_supported(i915) || !vrr_output_is_dp(o) || conn < 0 || (uint32_t)conn >= dev->nconn)
		return;
	rc = drm_connector_attach_vrr_capable_property(&dev->conn[conn]);
	if (rc)
		kprintf("[drm] i915: connector %d: vrr_capable not attached (%d)\n", conn, rc);
}

int intel_vrr_check(struct i915_device *i915, struct intel_output *o,
		    struct drm_crtc_state *cs)
{
	/* Never refused: a sink without a range, or a mode outside it, runs
	 * at the mode's fixed refresh whatever VRR_ENABLED says, as a
	 * fixed-rate sink would.  The timing itself is worked out again
	 * from the committed mode when the pipe runs. */
	(void)i915;
	(void)o;
	(void)cs;
	return 0;
}

void intel_vrr_link_prepare(struct i915_device *i915, struct intel_output *o,
			    const struct drm_crtc_state *cs)
{
	struct vrr_config c;

	if (!vrr_supported(i915) || !vrr_output_is_dp(o) || !cs)
		return;
	vrr_compute(i915, o, &cs->adjusted_mode, 0, &c);
	if (!!o->msa_timing_par_ignore != !!c.in_range)
		(void)intel_dp_set_msa_timing_par_ignore(o, c.in_range != 0);
}

void intel_vrr_enable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p,
		      const struct drm_crtc_state *cs)
{
	struct vrr_config c;
	static int msa_warned;

	if (!vrr_supported(i915) || !vrr_output_is_dp(o) || !p || !cs)
		return;
	vrr_compute(i915, o, &p->mode, cs->vrr_enabled, &c);
	/* The sink ignores the MSA timing whenever its range holds the
	 * mode: normally recorded before the link trained; corrected on the
	 * live link otherwise. */
	if (!!o->msa_timing_par_ignore != !!c.in_range &&
	    intel_dp_set_msa_timing_par_ignore(o, c.in_range != 0) < 0 && c.enable) {
		/* a sink still following the MSA timing would lose the
		 * picture once the frame length varies */
		if (!msa_warned++)
			kprintf("[drm] i915: port %c: the sink was not told to ignore the MSA timing, no variable refresh\n",
				'A' + o->port);
		return;
	}
	if (!c.enable)
		return;
	if (p->vrr.enabled) {
		if (vrr_same_timing(&p->vrr, &c))
			return;
		vrr_pipe_off(i915, p);
	}
	/* A transcoder set up while the sink had no range (or another
	 * guard band): set up now, on the running pipe. */
	if (i915->info->display_ver < 30 &&
	    (!p->vrr.in_range || p->vrr.guardband != (uint32_t)c.guardband ||
	     p->vrr.pipeline_full != (uint32_t)c.pipeline_full))
		vrr_set_transcoder_timings(i915, p, &c);
	vrr_hw_enable(i915, p, &c);
	vrr_store(p, &c);
	/* the range is in place before the vblank code uses it */
	vrr_barrier();
	p->vrr.enabled = 1;
}

void intel_vrr_disable(struct i915_device *i915, struct intel_output *o, struct intel_pipe *p,
		       const struct drm_crtc_state *cs)
{
	struct vrr_config c;

	if (!vrr_supported(i915) || !p || !p->vrr.enabled)
		return;
	if (cs) {
		/* still asked for, with the same range: it stays on */
		vrr_compute(i915, o, &p->mode, cs->vrr_enabled, &c);
		if (c.enable && vrr_same_timing(&p->vrr, &c))
			return;
	}
	vrr_pipe_off(i915, p);
	/* The pipe goes off: the sink follows the MSA timing again. */
	if (!cs && vrr_output_is_dp(o) && o->msa_timing_par_ignore)
		(void)intel_dp_set_msa_timing_par_ignore(o, false);
}

void intel_vrr_send_push(struct i915_device *i915, struct intel_pipe *p)
{
	if (!vrr_supported(i915) || !p || !p->vrr.enabled)
		return;
	i915_write32(i915, TRANS_PUSH(p->transcoder), TRANS_PUSH_EN | TRANS_PUSH_SEND);
}

void intel_vrr_connector_update(struct i915_device *i915, struct intel_output *o, int conn)
{
	struct drm_device *dev = &i915->drm;
	struct drm_connector *c;
	struct intel_vrr_output_state s = { 0 };

	if (!o || !vrr_supported(i915) || conn < 0 || (uint32_t)conn >= dev->nconn)
		return;
	c = &dev->conn[conn];
	if (o->detected && vrr_is_capable(i915, o, &c->display_info)) {
		s.capable = 1;
		s.min_vrefresh = c->display_info.monitor_range.min_vfreq;
		s.max_vrefresh = c->display_info.monitor_range.max_vfreq;
	}
	if (s.capable && (!o->vrr.capable || o->vrr.min_vrefresh != s.min_vrefresh ||
			  o->vrr.max_vrefresh != s.max_vrefresh))
		kprintf("[drm] i915: connector %d: variable refresh %u-%u Hz\n", conn,
			s.min_vrefresh, s.max_vrefresh);
	o->vrr = s;
	if (!vrr_output_is_dp(o))
		return;
	/* attached at display init; here too for a connector made without
	 * that (attaching twice does nothing) */
	if (!c->vrr_capable_property)
		intel_vrr_connector_init(i915, o, conn);
	drm_connector_set_vrr_capable_property(c, s.capable != 0);
}
