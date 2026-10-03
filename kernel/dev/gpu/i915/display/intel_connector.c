// LikeOS -- the connectors of the Intel display: detection and the mode
// list.
//
// A connector is one output (struct intel_output) on one DDI port: what
// is on it is asked of the sink (DPCD over AUX for DisplayPort, the
// hotplug pin and DDC for HDMI and DVI), and the modes come from its
// EDID -- on an embedded panel the panel's own timing first and the
// standard modes up to its size, scaled onto it by the pipe.  All of it
// runs under the display lock: the AUX channels, the panel power
// sequencer and the Type-C PHYs are shared with the mode set and the
// hotplug worker.
//
// With I915_FEAT_PROBE_HELPER the core's probe helper builds the list:
// detect, get_modes (the EDID's modes, or the panel timing the VBT
// gives), then every mode through intel_mode_valid() -- the transcoder's
// timing limits, the dot clock CDCLK allows, the DisplayPort link and the
// branch device in front of the sink, the HDMI port's TMDS limits, the
// panel scaler's bounds -- with the refused ones and the duplicates
// dropped and the rest sorted, the preferred mode first.  Without it the
// driver fills the list itself, adding the standard modes the link or
// the TMDS clock can carry, as before.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's and Dave Airlie's code: MIT
// Portions Copyright (C) 2019 Intel Corporation
// Portions Copyright (C) 2008 Intel Corporation
// Portions Copyright (C) 2006-2010 Intel Corporation
// Portions Copyright (c) 2006 Dave Airlie <airlied@linux.ie>
// Portions Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
// Portions Copyright (c) 2007, 2010 Intel Corporation
//   Jesse Barnes <jesse.barnes@intel.com>

#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/dev/gpu/i915/i915_reg.h>
#include <kernel/dev/gpu/i915/intel_display.h>
#include <kernel/dev/gpu/i915/intel_display_legacy.h>
#include <kernel/dev/gpu/i915/intel_connector.h>
#include <kernel/dev/gpu/i915/intel_features.h>
#include <kernel/dev/gpu/i915/intel_hdmi_feat.h>
#include <kernel/dev/gpu/i915/intel_vrr.h>
#include <kernel/dev/gpu/i915/intel_wm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_modes.h>
#include <kernel/dev/gpu/drm_probe_helper.h>
#include <kernel/dev/gpu/gpu_util.h>

static int detect_locked(struct drm_device *dev, struct drm_connector *c)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_output *o = intel_output_for_conn(i915, c);
	if (!o)
		return DRM_MODE_DISCONNECTED;
	/* a Type-C port answers only once the display holds its PHY */
	if (o->is_tc && !o->active && intel_tc_connect(i915, o))
		return DRM_MODE_DISCONNECTED;
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP)
		return intel_dp_detect(i915, o) ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI)
		return intel_hdmi_detect(i915, o) ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	return DRM_MODE_DISCONNECTED;
}

int intel_detect(struct drm_device *dev, struct drm_connector *c)
{
	if (intel_legacy_display_active(to_i915(dev)))
		return intel_legacy_detect(dev, c);
	struct i915_device *i915 = to_i915(dev);
	int rc;

	intel_display_lock(i915);
	rc = detect_locked(dev, c);
	/* nothing there any more: no refresh range either (vrr_capable
	 * drops; a probe that finds a sink sets it from the EDID) */
	if (rc == DRM_MODE_DISCONNECTED) {
		struct intel_output *o = intel_output_for_conn(i915, c);
		if (o)
			intel_vrr_connector_update(i915, o, (int)(c - dev->conn));
	}
	intel_display_unlock(i915);
	return rc;
}

/* Does the core's probe helper build this device's mode lists? */
static int probe_helper_on(const struct drm_device *dev)
{
	return dev->drv && (dev->drv->features & DRM_FEATURE_PROBE_HELPER) &&
	       dev->drv->mode_valid == intel_mode_valid;
}

static int get_modes_locked(struct drm_device *dev, struct drm_connector *c)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_output *o = intel_output_for_conn(i915, c);
	int conn = (int)(c - dev->conn);
	int helper = probe_helper_on(dev);
	int n = 0;

	if (!o)
		return (int)c->nmodes;
	if (!o->detected) {
		intel_vrr_connector_update(i915, o, conn);
		return (int)c->nmodes;
	}
	if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP) {
		if (intel_dp_read_edid(i915, o) == 0)
			n = drm_connector_set_edid(dev, conn, o->edid, (unsigned)o->edid_len);
	} else if (o->edid_len > 0 || intel_hdmi_read_edid(i915, o) == 0) {
		n = drm_connector_set_edid(dev, conn, o->edid, (unsigned)o->edid_len);
	}
	if (n <= 0) {
		drm_connector_set_edid(dev, conn, NULL, 0);
		drm_connector_clear_modes(dev, conn);
		/* the VBT's timing is marked preferred: it stays first */
		if (o->is_edp && i915->display.vbt.panel_mode_valid) {
			drm_connector_add_mode(dev, conn, &i915->display.vbt.panel_mode);
			n = 1;
		}
	}
	if (o->is_edp) {
		/* The panel's timing is the first mode; anything else on the
		 * list is scaled onto it by the pipe, so the standard modes
		 * up to its size are offered too. */
		o->fixed_mode_valid = 0;
		if (c->nmodes) {
			o->fixed_mode = c->modes[0];
			o->fixed_mode_valid = 1;
			if (!helper || I915_FEAT_EDP_STD_MODES)
				drm_connector_add_std_modes(dev, conn, o->fixed_mode.clock,
							    o->fixed_mode.hdisplay,
							    o->fixed_mode.vdisplay);
		}
	} else if (!helper) {
		/* an external sink: the standard modes under its limits */
		uint32_t max_clock;
		if (o->type == INTEL_OUTPUT_DP)
			max_clock = o->nsink_rates ?
					    o->sink_rates_khz[o->nsink_rates - 1] * intel_dp_max_lanes(o) / 3 :
					    165000;
		else if (I915_FEAT_HDMI_SCRAMBLING)
			/* the source's, the board's, the DP++ adaptor's and
			 * the sink's TMDS limits; above 340 MHz only where
			 * both ends scramble */
			max_clock = intel_hdmi_port_clock_limit(i915, o,
								o->type == INTEL_OUTPUT_HDMI &&
									o->hdmi_sink);
		else
			max_clock = (o->type == INTEL_OUTPUT_HDMI && o->hdmi_sink) ? 300000 : 165000;
		drm_connector_add_std_modes(dev, conn, max_clock, 4096, 2304);
	}
	/* the sink's refresh range, from the EDID just parsed */
	intel_vrr_connector_update(i915, o, conn);
	return (int)c->nmodes;
}

int intel_get_modes(struct drm_device *dev, struct drm_connector *c)
{
	if (intel_legacy_display_active(to_i915(dev)))
		return intel_legacy_get_modes(dev, c);
	struct i915_device *i915 = to_i915(dev);
	int rc;

	intel_display_lock(i915);
	rc = get_modes_locked(dev, c);
	intel_display_unlock(i915);
	return rc;
}

void intel_connector_probe(struct i915_device *i915, int conn)
{
	struct drm_device *dev = &i915->drm;
	struct drm_connector *c;

	if (conn < 0 || (uint32_t)conn >= dev->nconn)
		return;
	c = &dev->conn[conn];
	if (probe_helper_on(dev)) {
		drm_helper_probe_single_connector_modes(dev, c, dev->max_width, dev->max_height);
		return;
	}
	int st = intel_detect(dev, c);
	c->connected = (st == DRM_MODE_CONNECTED);
	if (c->connected)
		intel_get_modes(dev, c);
}

/* ---- the probe helper's mode check -------------------------------------------- */

/* The fastest pixel clock one pipe can be fed at the platform's highest
 * CDCLK: one pixel per clock, two from display version 10, all of the
 * clock usable on the DDI displays.  There is no joining of pipes here,
 * so this is the ceiling of every mode.  From display version 30 the
 * uncompressed pipe has a lower limit of its own. */
static int max_dotclock_khz(struct i915_device *i915)
{
	int ppc = i915->info->display_ver >= 10 ? 2 : 1;
	int guardband = 100;
	int max_dotclock = (int)((uint64_t)intel_cdclk_max_khz(i915) * (uint32_t)ppc * (uint32_t)guardband / 100);
	int limit = max_dotclock;

	if (intel_display_verx100(i915) == 3002)
		limit = 937500;
	else if (i915->info->display_ver >= 30)
		limit = 1350000;
	return min(max_dotclock, limit);
}

/* What the transcoder can time at all, whatever the output. */
static enum drm_mode_status device_mode_valid(struct i915_device *i915,
					      const struct drm_display_mode *mode)
{
	int hdisplay_max, htotal_max, vdisplay_max, vtotal_max;
	int max_dotclk;

	/* A doublescan mode is refused by each output below rather than
	 * here: never on a connector's list. */
	if (mode->vscan > 1)
		return MODE_NO_VSCAN;
	if (mode->flags & DRM_MODE_FLAG_HSKEW)
		return MODE_H_ILLEGAL;
	if (mode->flags & (DRM_MODE_FLAG_CSYNC | DRM_MODE_FLAG_NCSYNC | DRM_MODE_FLAG_PCSYNC))
		return MODE_HSYNC;
	if (mode->flags & (DRM_MODE_FLAG_BCAST | DRM_MODE_FLAG_PIXMUX | DRM_MODE_FLAG_CLKDIV2))
		return MODE_BAD;
	if (mode->clock <= 0)
		return MODE_CLOCK_LOW;

	/* Excessive dot clocks out first: the arithmetic below then stays
	 * small.  0 while the CDCLK limits are not known. */
	max_dotclk = max_dotclock_khz(i915);
	if (max_dotclk > 0 && mode->clock > max_dotclk)
		return MODE_CLOCK_HIGH;

	/* the transcoder's timing limits */
	if (i915->info->display_ver >= 11) {
		hdisplay_max = 16384;
		vdisplay_max = 8192;
		htotal_max = 16384;
		vtotal_max = 8192;
	} else {
		hdisplay_max = 8192;
		vdisplay_max = 4096;
		htotal_max = 8192;
		vtotal_max = 8192;
	}
	if (mode->hdisplay > hdisplay_max || mode->hsync_start > htotal_max ||
	    mode->hsync_end > htotal_max || mode->htotal > htotal_max)
		return MODE_H_ILLEGAL;
	if (mode->vdisplay > vdisplay_max || mode->vsync_start > vtotal_max ||
	    mode->vsync_end > vtotal_max || mode->vtotal > vtotal_max)
		return MODE_V_ILLEGAL;

	/* The line time the watermarks are programmed with goes up to
	 * (almost) 64 us. */
	if (DIV_ROUND_UP((uint32_t)mode->htotal * 1000u, (uint32_t)mode->clock) > 64)
		return MODE_H_ILLEGAL;
	return MODE_OK;
}

/* The transcoder's blanking needs, and the front porch it cannot do
 * without. */
static enum drm_mode_status cpu_transcoder_mode_valid(const struct drm_display_mode *mode)
{
	if (mode->hdisplay < 64 || mode->htotal - mode->hdisplay < 32)
		return MODE_H_ILLEGAL;
	if (mode->vtotal - mode->vdisplay < 5)
		return MODE_V_ILLEGAL;
	/* a horizontal front porch of 0 is not taken */
	if (mode->hsync_start == mode->hdisplay)
		return MODE_H_ILLEGAL;
	return MODE_OK;
}

/* A plane as large as the mode, for a full-screen picture. */
static enum drm_mode_status max_plane_size_mode_valid(struct i915_device *i915,
						      const struct drm_display_mode *mode)
{
	int plane_width_max, plane_height_max;

	if (i915->info->display_ver < 9)
		return MODE_OK;
	if (i915->info->display_ver >= 30) {
		plane_width_max = 6144;
		plane_height_max = 4800;
	} else if (i915->info->display_ver >= 11) {
		plane_width_max = 5120;
		plane_height_max = 4320;
	} else {
		plane_width_max = 5120;
		plane_height_max = 4096;
	}
	if (mode->hdisplay > plane_width_max)
		return MODE_H_ILLEGAL;
	if (mode->vdisplay > plane_height_max)
		return MODE_V_ILLEGAL;
	return MODE_OK;
}

/* The embedded panel: it runs its own timing (o->fixed_mode).  A mode of
 * the panel's size is driven as it is; with I915_FEAT_EDP_STD_MODES any
 * other is scaled onto the panel within the bounds the atomic check
 * accepts, else only the panel's own size at its refresh rate is
 * offered.  *hw is the timing the transcoder will run. */
static enum drm_mode_status panel_mode_valid(const struct intel_output *o,
					     const struct drm_connector *c,
					     const struct drm_display_mode *mode,
					     struct drm_mode_modeinfo *hw)
{
	const struct drm_mode_modeinfo *f = &o->fixed_mode;

	drm_mode_to_umode(hw, mode);
	if (!o->fixed_mode_valid) {
		/* Nothing tells the panel's timing (no EDID, no VBT mode):
		 * the standard modes the probe would offer instead are not
		 * the panel's, and none of them is driven onto it. */
		if (!c->edid_blob_id)
			return MODE_PANEL;
		return MODE_OK;
	}
	if (mode->hdisplay == f->hdisplay && mode->vdisplay == f->vdisplay) {
		if (!I915_FEAT_EDP_STD_MODES) {
			struct drm_display_mode fm;

			if (drm_mode_from_umode(&fm, f) == 0 &&
			    drm_mode_vrefresh(mode) != drm_mode_vrefresh(&fm))
				return MODE_PANEL;
		}
		return MODE_OK;
	}
	if (!I915_FEAT_EDP_STD_MODES)
		return MODE_PANEL;
	/* the pipe scaler's bounds, as intel_atomic_check() applies them */
	if (mode->hdisplay < 8 || mode->vdisplay < 8 || mode->hdisplay > f->hdisplay * 3 ||
	    mode->vdisplay > f->vdisplay * 3 || mode->hdisplay > 4096 || mode->vdisplay > 4096)
		return MODE_PANEL;
	*hw = *f;
	return MODE_OK;
}

static enum drm_mode_status dp_mode_valid(struct i915_device *i915, struct intel_output *o,
					  const struct drm_connector *c,
					  const struct drm_display_mode *mode)
{
	struct drm_mode_modeinfo hw;
	enum drm_mode_status status;

	status = cpu_transcoder_mode_valid(mode);
	if (status != MODE_OK)
		return status;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;
	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		return MODE_H_ILLEGAL;
	if (mode->clock < 10000)
		return MODE_CLOCK_LOW;

	if (o->is_edp) {
		status = panel_mode_valid(o, c, mode, &hw);
		if (status != MODE_OK)
			return status;
	} else {
		drm_mode_to_umode(&hw, mode);
	}

	/* The link: as the atomic check decides it -- uncompressed at the
	 * fastest rate and every lane the sink and the port share, else
	 * compressed where both ends can. */
	if (o->detected && !intel_dp_link_carries(i915, o, &hw)) {
		struct intel_dsc_config dsc;

		if (intel_dsc_compute_config(i915, o, &hw, &dsc))
			return MODE_CLOCK_HIGH;
	}

	status = max_plane_size_mode_valid(i915, mode);
	if (status != MODE_OK)
		return status;

	/* the dot clock the pipe runs, the panel's when it scales */
	{
		int max_dotclk = max_dotclock_khz(i915);
		if (max_dotclk > 0 && (int)hw.clock > max_dotclk)
			return MODE_CLOCK_HIGH;
	}

	/* A branch device in front of the sink (a DP-to-HDMI/DVI/VGA
	 * adaptor) passes on only what its own dot and TMDS clock limits
	 * allow; nothing of the kind on a panel. */
	if (!o->is_edp)
		return intel_dp_mode_valid_downstream(i915, o, mode);
	return MODE_OK;
}

static enum drm_mode_status hdmi_mode_valid(struct i915_device *i915, struct intel_output *o,
					    const struct drm_display_mode *mode)
{
	struct drm_mode_modeinfo m;
	enum drm_mode_status status;

	status = cpu_transcoder_mode_valid(mode);
	if (status != MODE_OK)
		return status;
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
		return MODE_NO_DBLESCAN;
	status = max_plane_size_mode_valid(i915, mode);
	if (status != MODE_OK)
		return status;
	/* the port's TMDS limits and clocks, as the atomic check asks
	 * them (only of a detected sink there) */
	drm_mode_to_umode(&m, mode);
	if (o->detected && intel_hdmi_mode_valid(i915, o, &m)) {
		if (mode->flags & DRM_MODE_FLAG_INTERLACE)
			return MODE_NO_INTERLACE;
		if (mode->clock < 25000)
			return MODE_CLOCK_LOW;
		return MODE_CLOCK_RANGE;
	}
	return MODE_OK;
}

enum drm_mode_status intel_mode_valid(struct drm_device *dev, struct drm_connector *c,
				      const struct drm_display_mode *mode)
{
	struct i915_device *i915 = to_i915(dev);
	struct intel_output *o;
	enum drm_mode_status status;

	/* the older displays judge their modes in their own code */
	if (intel_legacy_display_active(i915))
		return MODE_OK;

	status = device_mode_valid(i915, mode);
	if (status != MODE_OK)
		return status;

	/* Called from the probe helper and from inside get_modes (the
	 * EDID's modes as drm_connector_set_edid() lists them): the lock
	 * nests. */
	intel_display_lock(i915);
	o = intel_output_for_conn(i915, c);
	if (!o)
		status = MODE_ERROR;
	else if (o->type == INTEL_OUTPUT_DP || o->type == INTEL_OUTPUT_EDP)
		status = dp_mode_valid(i915, o, c, mode);
	else if (o->type == INTEL_OUTPUT_HDMI || o->type == INTEL_OUTPUT_DVI)
		status = hdmi_mode_valid(i915, o, mode);
	else
		status = MODE_ERROR;
	intel_display_unlock(i915);
	return status;
}

void intel_connector_driver_setup(struct i915_device *i915, struct drm_driver *drv)
{
	drv->detect = intel_detect;
	drv->get_modes = intel_get_modes;
	/* The older displays keep filling their lists themselves. */
	if (I915_FEAT_PROBE_HELPER && !intel_display_legacy_part(i915)) {
		drv->mode_valid = intel_mode_valid;
		drv->features |= DRM_FEATURE_PROBE_HELPER;
	}
	/* IN_FENCE_FD / OUT_FENCE_PTR: the core waits for the in-fences
	 * before the commit reaches the driver and signals the out-fences
	 * at the vblank the commit's flip completes at, for every part
	 * (the older displays commit through the same atomic interface). */
	if (I915_FEAT_ATOMIC_FENCES && drv->atomic_check && drv->atomic_commit)
		drv->features |= DRM_FEATURE_ATOMIC_FENCES;
}
