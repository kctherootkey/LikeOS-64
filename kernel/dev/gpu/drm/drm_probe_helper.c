// LikeOS -- display-manager core: probing a connector for its sink and modes.
//
// What a probe does, step by step (drm_helper_probe_single_connector_modes):
//
//  1. Ask the driver whether a sink is there (detect).  A change of status
//     bumps the connector's epoch counter and, because a client may have
//     probed before the driver's hotplug code noticed, the device's hotplug
//     count, so that whoever watches it looks again.
//  2. A disconnected connector loses its EDID and its modes; done.
//  3. The driver fills the mode list (get_modes).  A connected sink the
//     driver found no modes for is offered the standard modes up to
//     1024x768 -- on DisplayPort with 640x480 preferred, the fail-safe mode
//     every DisplayPort sink must take.
//  4. Every mode is checked: the core's sanity checks, the device's size
//     limits, interlace / double scan / stereo / YCbCr 4:2:0-only where the
//     connector cannot send them, and the driver's own verdict
//     (drm_driver.mode_valid).  Equal modes are merged, refused ones
//     dropped.  On DisplayPort an empty result gets 640x480 once more.
//  5. The rest are sorted best first: the preferred mode first, then the
//     larger, the higher refresh rate, the higher clock.
//
// The connector keeps its list in the form clients see (struct
// drm_mode_modeinfo, at most DRM_MAX_MODES); the steps work on the core's
// form (struct drm_display_mode) in an allocated array -- a few hundred of
// them do not belong on a kernel stack.
//
// Hotplug: a driver's interrupt path calls drm_connector_hotplug() (it
// re-probes and logs) or the helpers here (detect only); either way the
// device's hotplug count in /sys is what tells clients.  No lock is taken
// here: a driver's detect and get_modes take the driver's own, and a
// driver's hotplug thread calls in while holding it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's and Dave Airlie's code: HPND-sell-variant
// Portions Copyright (C) 2006-2008 Intel Corporation
// Portions Copyright (C) 2007 Dave Airlie <airlied@linux.ie>

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_probe_helper.h>
#include <kernel/uapi/bug.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* ---- checking one mode ------------------------------------------------------- */

static enum drm_mode_status drm_mode_validate_flag(const struct drm_display_mode *mode,
						   int flags)
{
	if ((mode->flags & DRM_MODE_FLAG_INTERLACE) &&
	    !(flags & DRM_MODE_FLAG_INTERLACE))
		return MODE_NO_INTERLACE;

	if ((mode->flags & DRM_MODE_FLAG_DBLSCAN) &&
	    !(flags & DRM_MODE_FLAG_DBLSCAN))
		return MODE_NO_DBLESCAN;

	if ((mode->flags & DRM_MODE_FLAG_3D_MASK) &&
	    !(flags & DRM_MODE_FLAG_3D_MASK))
		return MODE_NO_STEREO;

	return MODE_OK;
}

static enum drm_mode_status mode_valid_for(struct drm_device *dev,
					   struct drm_connector *c,
					   const struct drm_display_mode *mode,
					   uint32_t maxX, uint32_t maxY)
{
	enum drm_mode_status status;
	int mode_flags = 0;

	if (c->interlace_allowed)
		mode_flags |= DRM_MODE_FLAG_INTERLACE;
	if (c->doublescan_allowed)
		mode_flags |= DRM_MODE_FLAG_DBLSCAN;
	if (c->stereo_allowed)
		mode_flags |= DRM_MODE_FLAG_3D_MASK;

	status = drm_mode_validate_driver(dev, mode);
	if (status != MODE_OK)
		return status;

	status = drm_mode_validate_size(mode, (int)maxX, (int)maxY);
	if (status != MODE_OK)
		return status;

	status = drm_mode_validate_flag(mode, mode_flags);
	if (status != MODE_OK)
		return status;

	/* A pixel-repeated mode (the 1440-wide CEA timings that show 720
	 * pixels) needs a source that repeats each pixel.  Only a driver
	 * that judges modes itself has said whether it can; the others have
	 * never been handed one. */
	if ((mode->flags & DRM_MODE_FLAG_DBLCLK) && !dev->drv->mode_valid)
		return MODE_H_ILLEGAL;

	status = drm_mode_validate_ycbcr420(mode, &c->display_info,
					    c->ycbcr_420_allowed);
	if (status != MODE_OK)
		return status;

	if (dev->drv->mode_valid)
		return dev->drv->mode_valid(dev, c, mode);
	return MODE_OK;
}

enum drm_mode_status drm_connector_mode_valid(struct drm_device *dev,
					      struct drm_connector *c,
					      const struct drm_display_mode *mode)
{
	return mode_valid_for(dev, c, mode, dev->max_width, dev->max_height);
}

/* ---- the list steps ---------------------------------------------------------- */

int drm_helper_probe_validate_list(struct drm_device *dev,
				   struct drm_connector *c,
				   struct drm_display_mode *modes, int n,
				   uint32_t maxX, uint32_t maxY)
{
	int out = 0;

	/* Merge equal modes: the first stays where it is and takes the
	 * other's type bits (and its timings, when only the other one is
	 * preferred).  `out' never passes `i', so nothing unread is
	 * overwritten. */
	for (int i = 0; i < n; i++) {
		bool dup = false;

		for (int j = 0; j < out; j++) {
			if (drm_display_mode_equal(&modes[j], &modes[i])) {
				dup = true;
				break;
			}
		}
		if (dup) {
			/* no room: it can only merge */
			drm_mode_list_update(modes, out, out, &modes[i], 1);
			continue;
		}
		if (out != i)
			drm_mode_copy(&modes[out], &modes[i]);
		out++;
	}
	n = out;

	for (int i = 0; i < n; i++)
		modes[i].status = mode_valid_for(dev, c, &modes[i], maxX, maxY);

	n = drm_mode_prune_invalid(modes, n, false);
	drm_mode_sort(modes, n);
	return n;
}

void drm_helper_probe_store_modes(struct drm_connector *c,
				  const struct drm_display_mode *modes, int n)
{
	uint32_t k = 0;

	for (int i = 0; i < n && k < DRM_MAX_MODES; i++)
		drm_mode_convert_to_umode(&c->modes[k++], &modes[i]);
	c->nmodes = k;
}

int drm_helper_probe_edid_list(struct drm_device *dev, struct drm_connector *c,
			       struct drm_display_mode *modes, int n,
			       const struct drm_edid_info *summary)
{
	int pref = -1;

	n = drm_helper_probe_validate_list(dev, c, modes, n, dev->max_width,
					   dev->max_height);

	/* One preferred mode, the one the summary chose: the first the EDID
	 * marks, else (before EDID 1.4) its first detailed timing, else the
	 * largest.  The console and the drivers take modes[0], and that is
	 * the mode they have always found there. */
	if (summary && summary->preferred >= 0 &&
	    summary->preferred < summary->nmodes) {
		for (int i = 0; i < n; i++) {
			struct drm_mode_modeinfo u;

			drm_mode_convert_to_umode(&u, &modes[i]);
			if (drm_mode_equal(&u, &summary->modes[summary->preferred])) {
				pref = i;
				break;
			}
		}
	}
	if (pref < 0 && n > 0)
		pref = 0; /* refused or none: the best that is left */
	if (pref >= 0) {
		for (int i = 0; i < n; i++)
			modes[i].type &= (uint8_t)~DRM_MODE_TYPE_PREFERRED;
		modes[pref].type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_sort(modes, n);
	}

	drm_helper_probe_store_modes(c, modes, n);
	return (int)c->nmodes;
}

/* The connector's list as core modes in a new array (kfree), *n of them;
 * NULL without memory.  Room for DRM_PROBE_MAX_MODES. */
static struct drm_display_mode *modes_load(struct drm_connector *c, int *n)
{
	struct drm_display_mode *modes = kcalloc(DRM_PROBE_MAX_MODES, sizeof(*modes));
	int k = 0;

	if (!modes)
		return NULL;
	for (uint32_t i = 0; i < c->nmodes && k < DRM_PROBE_MAX_MODES; i++) {
		/* a picture aspect value no client form knows: not a mode */
		if (drm_mode_from_umode(&modes[k], &c->modes[i]) == 0)
			k++;
	}
	*n = k;
	return modes;
}

static int finish_modes(struct drm_device *dev, struct drm_connector *c,
			uint32_t maxX, uint32_t maxY)
{
	struct drm_display_mode *modes;
	int n = 0;

	modes = modes_load(c, &n);
	if (!modes)
		return (int)c->nmodes;
	n = drm_helper_probe_validate_list(dev, c, modes, n, maxX, maxY);
	drm_helper_probe_store_modes(c, modes, n);
	kfree(modes);
	return (int)c->nmodes;
}

int drm_helper_probe_finish_modes(struct drm_device *dev, struct drm_connector *c)
{
	return finish_modes(dev, c, dev->max_width, dev->max_height);
}

/* The standard modes up to hdisplay x vdisplay at no more than 61 Hz,
 * appended to the connector's list. */
static int add_modes_noedid(struct drm_connector *c, unsigned int hdisplay,
			    unsigned int vdisplay)
{
	struct drm_edid_conn ec;
	struct drm_display_mode *modes;
	int added = 0;

	modes = kcalloc(DRM_PROBE_MAX_MODES, sizeof(*modes));
	if (!modes)
		return 0;
	mm_memset(&ec, 0, sizeof(ec));
	ec.connector_type = c->type;
	ec.info = &c->display_info;
	ec.modes = modes;
	ec.max_modes = DRM_PROBE_MAX_MODES;
	drm_add_modes_noedid(&ec, hdisplay, vdisplay);
	for (int i = 0; i < ec.nmodes && c->nmodes < DRM_MAX_MODES; i++) {
		drm_mode_convert_to_umode(&c->modes[c->nmodes++], &modes[i]);
		added++;
	}
	kfree(modes);
	return added;
}

/* Mark the connector's modes of hpref x vpref preferred. */
static void set_preferred_mode(struct drm_connector *c, int hpref, int vpref)
{
	for (uint32_t i = 0; i < c->nmodes; i++)
		if (c->modes[i].hdisplay == hpref && c->modes[i].vdisplay == vpref)
			c->modes[i].type |= DRM_MODE_TYPE_PREFERRED;
}

/* ---- detect -------------------------------------------------------------------- */

int drm_helper_probe_detect(struct drm_device *dev, struct drm_connector *c,
			    bool force)
{
	int status;

	(void)force; /* the drivers' detect always looks */
	if (!dev->drv->detect)
		return DRM_MODE_CONNECTED;
	status = dev->drv->detect(dev, c);
	if (status != DRM_MODE_CONNECTED && status != DRM_MODE_DISCONNECTED &&
	    status != DRM_MODE_UNKNOWNCONNECTION) {
		WARN_RATELIMIT(1, "drm: connector %u: detect returned %d", c->id,
			       status);
		status = DRM_MODE_UNKNOWNCONNECTION;
	}
	if ((status == DRM_MODE_CONNECTED) != (c->connected != 0))
		c->epoch_counter++;
	return status;
}

/* ---- the probe ------------------------------------------------------------------ */

int drm_helper_probe_single_connector_modes(struct drm_device *dev,
					    struct drm_connector *c,
					    uint32_t maxX, uint32_t maxY)
{
	int conn = (int)(c - dev->conn);
	int old_connected = c->connected;
	int status, count = 0;

	status = drm_helper_probe_detect(dev, c, true);
	c->connected = (status == DRM_MODE_CONNECTED);

	/* Normally the driver's hotplug code reports a change; a client's
	 * probe may get there first, so say it here too. */
	if (old_connected != c->connected)
		drm_kms_helper_hotplug_event(dev);

	if (status == DRM_MODE_DISCONNECTED) {
		drm_connector_set_edid(dev, conn, NULL, 0);
		drm_connector_clear_modes(dev, conn);
		return 0;
	}

	if (dev->drv->get_modes) {
		count = dev->drv->get_modes(dev, c);
		/* get_modes says how many; a negative answer is a bug */
		if (count < 0) {
			WARN_RATELIMIT(1, "drm: connector %u: get_modes returned %d",
				       c->id, count);
			count = 0;
		}
	} else {
		count = (int)c->nmodes;
	}

	if (count == 0) {
		count = add_modes_noedid(c, 1024, 768);
		/* DisplayPort's fail-safe mode is 640x480: without an EDID
		 * that is where a sink must be met. */
		if (c->type == DRM_MODE_CONNECTOR_DisplayPort)
			set_preferred_mode(c, 640, 480);
	}
	if (count != 0)
		finish_modes(dev, c, maxX, maxY);

	/* Every detachable DisplayPort sink takes 640x480 at 60 Hz: if all
	 * modes were refused (a link too slow for them, say), try that. */
	if (c->nmodes == 0 && c->type == DRM_MODE_CONNECTOR_DisplayPort) {
		count = add_modes_noedid(c, 640, 480);
		finish_modes(dev, c, maxX, maxY);
	}

	return count;
}

/* ---- hotplug -------------------------------------------------------------------- */

void drm_kms_helper_hotplug_event(struct drm_device *dev)
{
	/* No event bus reaches clients here: the count in /sys does. */
	__atomic_add_fetch(&dev->hotplug_epoch, 1, __ATOMIC_RELEASE);
}

void drm_kms_helper_connector_hotplug_event(struct drm_connector *c)
{
	if (WARN_ON(!c->dev))
		return;
	drm_kms_helper_hotplug_event(c->dev);
}

static bool check_connector_changed(struct drm_device *dev,
				    struct drm_connector *c)
{
	uint64_t old_epoch_counter = c->epoch_counter;
	int status;

	status = drm_helper_probe_detect(dev, c, false);
	c->connected = (status == DRM_MODE_CONNECTED);

	return old_epoch_counter != c->epoch_counter;
}

bool drm_connector_helper_hpd_irq_event(struct drm_device *dev,
					struct drm_connector *c)
{
	bool changed = check_connector_changed(dev, c);

	if (changed)
		drm_kms_helper_connector_hotplug_event(c);
	return changed;
}

bool drm_helper_hpd_irq_event(struct drm_device *dev)
{
	bool changed = false;

	for (uint32_t i = 0; i < dev->nconn; i++)
		if (check_connector_changed(dev, &dev->conn[i]))
			changed = true;
	if (changed)
		drm_kms_helper_hotplug_event(dev);
	return changed;
}

/* ---- fixed panels ----------------------------------------------------------------- */

enum drm_mode_status drm_crtc_helper_mode_valid_fixed(const struct drm_display_mode *mode,
						      const struct drm_display_mode *fixed_mode)
{
	if (mode->hdisplay != fixed_mode->hdisplay &&
	    mode->vdisplay != fixed_mode->vdisplay)
		return MODE_ONE_SIZE;
	else if (mode->hdisplay != fixed_mode->hdisplay)
		return MODE_ONE_WIDTH;
	else if (mode->vdisplay != fixed_mode->vdisplay)
		return MODE_ONE_HEIGHT;

	return MODE_OK;
}

int drm_connector_helper_get_modes_fixed(struct drm_device *dev,
					 struct drm_connector *c,
					 const struct drm_display_mode *fixed_mode)
{
	struct drm_display_mode mode;
	struct drm_mode_modeinfo u;

	drm_mode_copy(&mode, fixed_mode);
	if (mode.name[0] == '\0')
		drm_mode_set_name(&mode);
	mode.type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_convert_to_umode(&u, &mode);
	if (drm_connector_add_mode(dev, (int)(c - dev->conn), &u) != 0)
		return 0;

	if (mode.width_mm) {
		c->display_info.width_mm = mode.width_mm;
		c->mm_width = mode.width_mm;
	}
	if (mode.height_mm) {
		c->display_info.height_mm = mode.height_mm;
		c->mm_height = mode.height_mm;
	}
	return 1;
}
