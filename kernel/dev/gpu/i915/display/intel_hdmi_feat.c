// LikeOS -- HDMI 2.0 link features of the Intel display: scrambling, the
// TMDS bit clock ratio and the sink's SCDC side of both, the TMDS limits
// of source, sink and DP++ adaptor, and the link check after a hotplug.
//
// Above a TMDS character rate of 340 MHz HDMI 2.0 scrambles the stream
// and runs the clock lane at a quarter of the character rate (the 1/40
// bit clock ratio).  The transcoder does both when its DDI function says
// so; the sink has to be told the same over SCDC (I2C address 0x54 on the
// DDC bus) before the port's buffer starts sending, and reports through
// SCDC whether its descrambler locked.  A sink that says so scrambles
// below 340 MHz too.  The source scrambles only where its own TMDS limit
// -- the platform's, or lower where the board's VBT says the port
// cannot do HDMI 2.0 rates (a retimer for HDMI 1.4, say) -- is above 340
// MHz.
//
// What the sink was told does not survive it going away: a sink pulled
// and plugged back while the output stays lit starts unscrambled at 1/10
// and shows nothing.  HDMI 2.0 wants the TMDS clock stopped while the
// ratio changes and nothing scrambled sent before the sink is set up, so
// the output needs a full mode set; the hotplug check marks the
// connector's link BAD for the client to do it.
//
// A DP++ (dual-mode) DisplayPort connector carries TMDS through an
// adaptor: a type 1 adaptor passes up to 165 MHz and may not answer on
// the bus at all, a type 2 one says its limit and has output buffers the
// source switches on and off.  On a port the VBT does not call DP++ only
// the buffers are switched (a VBT can be wrong about an adaptor's limit,
// not about a port that is native HDMI).
//
// Switches (intel_features.h): I915_FEAT_HDMI_SCRAMBLING (the limits and
// scrambling; 0: every link a plain one, clamped at 340 MHz),
// I915_FEAT_HDMI_LOW_RATE_SCRAMBLING, I915_FEAT_DP_DUAL_MODE.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: MIT
// Portions Copyright (C) 2006 Dave Airlie <airlied@linux.ie>
// Portions Copyright (C) 2006-2009 Intel Corporation
// Portions Copyright (C) 2012 Intel Corporation

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_dpll_regs.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/i915/intel_hdmi_feat.h>
#include <kernel/dev/gpu/drm_display_info.h>
#include <kernel/dev/gpu/drm_scdc_helper.h>
#include <kernel/dev/gpu/drm_dp_dual_mode_helper.h>
#include <kernel/dev/gpu/drm_probe_helper.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>

/* HDMI 2.0, scrambling control: the sink has 100 ms to lock onto the
 * scrambled stream after it starts; the source polls for up to 200. */
#define HDMI_SCRAMBLING_POLL_MS 200

static const char *port_letter(const struct intel_output *o, char *buf)
{
	buf[0] = (char)('A' + o->port);
	buf[1] = 0;
	return buf;
}

/* The connector of an output, NULL before it has one. */
static struct drm_connector *output_connector(struct i915_device *i915,
					      const struct intel_output *o)
{
	if (o->conn < 0 || (uint32_t)o->conn >= i915->drm.nconn)
		return NULL;
	return &i915->drm.conn[o->conn];
}

/* What the sink's EDID says about it (all zero without an EDID). */
static const struct drm_display_info *sink_info(struct i915_device *i915,
						const struct intel_output *o)
{
	struct drm_connector *c = output_connector(i915, o);

	return c ? &c->display_info : NULL;
}

static int sink_scrambles(struct i915_device *i915, const struct intel_output *o)
{
	const struct drm_display_info *info = sink_info(i915, o);

	return info && info->hdmi.scdc.scrambling.supported;
}

void intel_hdmi_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	(void)i915;
	(void)drv;
}

/* ---- limits ----------------------------------------------------------------- */

uint32_t intel_hdmi_source_max_tmds_khz(struct i915_device *i915, const struct intel_output *o)
{
	uint32_t max = intel_dpll_hdmi_max_tmds_khz(i915);
	const struct intel_vbt *vbt = &i915->display.vbt;

	if (vbt->valid && o->port >= 0 && o->port < INTEL_MAX_PORTS) {
		uint32_t board = vbt->port[o->port].hdmi_max_tmds_khz;
		if (board && board < max)
			max = board;
	}
	return max;
}

/* HDMI 2.0 parts (display version 10 on) scramble; but a board with an
 * HDMI 1.4 retimer behind such a part has its VBT cap the port below
 * 340 MHz, and an HDMI 2.0 sink on it must not be sent a scrambled
 * stream the retimer cannot pass.  So the limit after the VBT decides. */
static int source_supports_scrambling(struct i915_device *i915, const struct intel_output *o)
{
	return intel_hdmi_source_max_tmds_khz(i915, o) > 340000;
}

static int vbt_dual_mode(struct i915_device *i915, const struct intel_output *o)
{
	const struct intel_vbt *vbt = &i915->display.vbt;

	return vbt->valid && o->port >= 0 && o->port < INTEL_MAX_PORTS &&
	       vbt->port[o->port].dp_dual_mode;
}

uint32_t intel_hdmi_port_clock_limit(struct i915_device *i915, const struct intel_output *o,
				     int hdmi_sink)
{
	const struct drm_display_info *info = sink_info(i915, o);
	uint32_t max = intel_hdmi_source_max_tmds_khz(i915, o);

	if (I915_FEAT_DP_DUAL_MODE && o->hdmi_link.dual_mode_max_tmds_khz &&
	    o->hdmi_link.dual_mode_max_tmds_khz < max)
		max = o->hdmi_link.dual_mode_max_tmds_khz;
	if (info && info->max_tmds_clock > 0) {
		if ((uint32_t)info->max_tmds_clock < max)
			max = (uint32_t)info->max_tmds_clock;
	} else if (!hdmi_sink && max > 165000) {
		max = 165000;
	}
	/* Above 340 MHz only scrambled, which both ends must do. */
	if (max > 340000 &&
	    !(hdmi_sink && source_supports_scrambling(i915, o) && sink_scrambles(i915, o)))
		max = 340000;
	return max;
}

/* ---- scrambling ------------------------------------------------------------- */

void intel_hdmi_link_compute(struct i915_device *i915, struct intel_output *o,
			     uint32_t clock_khz)
{
	const struct drm_display_info *info;

	o->hdmi_link.scrambling = 0;
	o->hdmi_link.high_tmds_clock_ratio = 0;
	if (!I915_FEAT_HDMI_SCRAMBLING || o->type != INTEL_OUTPUT_HDMI)
		return;
	info = sink_info(i915, o);
	if (!info || !info->hdmi.scdc.scrambling.supported ||
	    !source_supports_scrambling(i915, o))
		return;
	if (I915_FEAT_HDMI_LOW_RATE_SCRAMBLING && info->hdmi.scdc.scrambling.low_rates)
		o->hdmi_link.scrambling = 1;
	if (clock_khz > 340000) {
		o->hdmi_link.scrambling = 1;
		o->hdmi_link.high_tmds_clock_ratio = 1;
	}
}

uint32_t intel_hdmi_trans_ddi_bits(struct i915_device *i915, const struct intel_output *o,
				   const struct intel_pipe *p,
				   const struct drm_mode_modeinfo *m)
{
	uint32_t bits = 0;

	(void)i915;
	(void)p;
	(void)m;
	if (!I915_FEAT_HDMI_SCRAMBLING)
		return 0;
	if (o->hdmi_link.scrambling)
		bits |= TRANS_DDI_HDMI_SCRAMBLING;
	if (o->hdmi_link.high_tmds_clock_ratio)
		bits |= TRANS_DDI_HIGH_TMDS_CHAR_RATE;
	return bits;
}

/* Tell the sink the clock ratio (1/40 or 1/10) and whether the stream
 * is scrambled -- before the port sends anything, since a sink that
 * sees no scrambled clock within 100 ms of being told may turn
 * descrambling off again.  Nothing for a sink without scrambling.
 * Returns 1 on success. */
static int handle_sink_scrambling(struct i915_device *i915, struct intel_output *o,
				  int high_tmds_clock_ratio, int scrambling)
{
	char pl[2];

	if (!sink_scrambles(i915, o))
		return 1;
	i915_dbg("[drm] i915: port %s: scrambling %s, TMDS bit clock ratio 1/%d\n",
		 port_letter(o, pl), scrambling ? "on" : "off", high_tmds_clock_ratio ? 40 : 10);
	return drm_scdc_set_high_tmds_clock_ratio(&o->ddc, high_tmds_clock_ratio != 0) &&
	       drm_scdc_set_scrambling(&o->ddc, scrambling != 0);
}

void intel_hdmi_pre_port_enable(struct i915_device *i915, struct intel_output *o,
				struct intel_pipe *p, const struct drm_mode_modeinfo *m)
{
	char pl[2];

	(void)p;
	(void)m;
	if (!I915_FEAT_HDMI_SCRAMBLING || o->type != INTEL_OUTPUT_HDMI)
		return;
	if (!handle_sink_scrambling(i915, o, o->hdmi_link.high_tmds_clock_ratio,
				    o->hdmi_link.scrambling))
		kprintf("[drm] i915: port %s: the sink did not take the scrambling / TMDS clock ratio setting\n",
			port_letter(o, pl));
}

void intel_hdmi_post_enable(struct i915_device *i915, struct intel_output *o,
			    struct intel_pipe *p)
{
	struct drm_connector *c;
	char pl[2];
	int locked = 0;

	(void)p;
	if (!I915_FEAT_HDMI_SCRAMBLING || o->type != INTEL_OUTPUT_HDMI)
		return;
	/* a full mode set: whatever the sink had lost, it was told again */
	if (o->hdmi_link.link_bad) {
		o->hdmi_link.link_bad = 0;
		c = output_connector(i915, o);
		if (c)
			drm_connector_set_link_status_property(c, DRM_MODE_LINK_STATUS_GOOD);
	}
	if (!o->hdmi_link.scrambling)
		return;
	/* A short busy wait per poll, as with the clock ratio: this is the
	 * mode set path.  The sink normally locks within a few ms. */
	for (int ms = 0; ms < HDMI_SCRAMBLING_POLL_MS; ms++) {
		if (drm_scdc_get_scrambling_status(&o->ddc)) {
			locked = 1;
			break;
		}
		lapic_delay_us(1000);
	}
	if (!locked)
		kprintf("[drm] i915: port %s: the sink did not report its descrambler locked\n",
			port_letter(o, pl));
}

void intel_hdmi_link_disable(struct i915_device *i915, struct intel_output *o)
{
	char pl[2];

	if (!I915_FEAT_HDMI_SCRAMBLING || o->type != INTEL_OUTPUT_HDMI)
		return;
	if (!handle_sink_scrambling(i915, o, 0, 0))
		i915_dbg("[drm] i915: port %s: could not reset the sink's scrambling / TMDS clock ratio\n",
			 port_letter(o, pl));
}

void intel_hdmi_link_clear(struct intel_output *o)
{
	o->hdmi_link.scrambling = 0;
	o->hdmi_link.high_tmds_clock_ratio = 0;
}

int intel_hdmi_hpd_check(struct i915_device *i915, struct intel_output *o)
{
	struct drm_connector *c;
	uint8_t config;
	char pl[2];
	int rc;

	if (!I915_FEAT_HDMI_SCRAMBLING || o->type != INTEL_OUTPUT_HDMI)
		return 0;
	c = output_connector(i915, o);
	if (!c || !c->connected || !o->active || o->hdmi_link.link_bad)
		return 0;
	if (!o->hdmi_link.high_tmds_clock_ratio && !o->hdmi_link.scrambling)
		return 0;
	rc = drm_scdc_readb(&o->ddc, SCDC_TMDS_CONFIG, &config);
	if (rc < 0) {
		kprintf("[drm] i915: port %s: reading the sink's TMDS configuration failed (%d)\n",
			port_letter(o, pl), rc);
		return 0;
	}
	if (!!(config & SCDC_TMDS_BIT_CLOCK_RATIO_BY_40) == !!o->hdmi_link.high_tmds_clock_ratio &&
	    !!(config & SCDC_SCRAMBLING_ENABLE) == !!o->hdmi_link.scrambling)
		return 0;
	/* HDMI 2.0 wants no scrambled data sent before the sink is set up
	 * for it and the TMDS clock stopped while the sink's clock ratio
	 * changes: some sinks would take new SCDC settings on the fly, but
	 * only a full mode set is right for all of them.  The client sees
	 * the link go bad with the hotplug event and sets the mode again. */
	kprintf("[drm] i915: port %s: the sink lost its scrambling setting; link marked bad\n",
		port_letter(o, pl));
	o->hdmi_link.link_bad = 1;
	drm_connector_set_link_status_property(c, DRM_MODE_LINK_STATUS_BAD);
	drm_kms_helper_hotplug_event(&i915->drm);
	return 0;
}

/* ---- DP++ adaptors ------------------------------------------------------------ */

void intel_hdmi_dual_mode_reset(struct intel_output *o)
{
	o->hdmi_link.dual_mode_type = DRM_DP_DUAL_MODE_NONE;
	o->hdmi_link.dual_mode_max_tmds_khz = 0;
}

void intel_hdmi_dual_mode_detect(struct i915_device *i915, struct intel_output *o)
{
	enum drm_dp_dual_mode_type type;
	int max;
	char pl[2];

	if (!I915_FEAT_DP_DUAL_MODE)
		return;
	type = drm_dp_dual_mode_detect(&i915->drm, &o->ddc);
	/* A type 1 DVI adaptor need not answer at all, and the state of its
	 * CONFIG1 pin cannot be read here: only the VBT saying the port is
	 * DP++ tells it from a native HDMI port. */
	if (type == DRM_DP_DUAL_MODE_UNKNOWN)
		type = vbt_dual_mode(i915, o) ? DRM_DP_DUAL_MODE_TYPE1_DVI : DRM_DP_DUAL_MODE_NONE;
	if (type == DRM_DP_DUAL_MODE_NONE)
		return;
	max = drm_dp_dual_mode_max_tmds_clock(&i915->drm, type, &o->ddc);
	o->hdmi_link.dual_mode_type = (uint8_t)type;
	o->hdmi_link.dual_mode_max_tmds_khz = max > 0 ? (uint32_t)max : 0;
	i915_dbg("[drm] i915: port %s: DP dual mode adaptor (%s), max TMDS clock %d kHz\n",
		 port_letter(o, pl), drm_dp_get_dual_mode_type_name(type), max);
	/* Older VBTs are often wrong; but on a port the VBT does not call
	 * DP++ the limit is not trusted (every part here is Haswell or
	 * later, whose VBT names its native HDMI ports reliably). */
	if (!vbt_dual_mode(i915, o)) {
		i915_dbg("[drm] i915: port %s: native HDMI port: the adaptor's limit is ignored\n",
			 port_letter(o, pl));
		o->hdmi_link.dual_mode_max_tmds_khz = 0;
	}
}

void intel_hdmi_dual_mode_set_tmds_output(struct i915_device *i915, struct intel_output *o,
					  int enable)
{
	char pl[2];

	if (!I915_FEAT_DP_DUAL_MODE || o->hdmi_link.dual_mode_type < DRM_DP_DUAL_MODE_TYPE2_DVI)
		return;
	i915_dbg("[drm] i915: port %s: %s the DP dual mode adaptor's TMDS output\n",
		 port_letter(o, pl), enable ? "enabling" : "disabling");
	(void)drm_dp_dual_mode_set_tmds_output(&i915->drm,
					       (enum drm_dp_dual_mode_type)o->hdmi_link.dual_mode_type,
					       &o->ddc, enable != 0);
}
