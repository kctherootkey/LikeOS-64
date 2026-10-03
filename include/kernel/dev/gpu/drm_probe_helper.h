// LikeOS -- display-manager core: asking a connector what is plugged in.
//
// A probe asks the driver whether a sink is there (detect), has it fill the
// connector's mode list (get_modes: the sink's EDID through
// drm_connector_set_edid(), fixed panel timings, the standard table), and
// then makes that list one a client can be offered: every mode checked by
// the core (sane timings, inside the device's size limits, interlace /
// double scan / stereo / YCbCr 4:2:0 only where the connector can send
// them) and by the driver (drm_driver.mode_valid), the refused ones and
// the duplicates dropped, and the rest sorted best first -- the preferred
// mode at modes[0], where the console and the drivers look for it.
//
// A driver opts into the whole probe with DRM_FEATURE_PROBE_HELPER; the
// others keep the list exactly as their get_modes filled it.  A status
// change -- seen by a probe or reported by a driver's hotplug interrupt --
// bumps the connector's epoch counter and the device's hotplug count that
// /sys shows clients.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's and Dave Airlie's code: HPND-sell-variant
// Portions Copyright (C) 2006-2008 Intel Corporation
// Portions Copyright (C) 2007 Dave Airlie <airlied@linux.ie>

#ifndef KERNEL_DEV_GPU_DRM_PROBE_HELPER_H
#define KERNEL_DEV_GPU_DRM_PROBE_HELPER_H

#include <kernel/dev/gpu/drm.h>

/* The most modes one probe works on before the list is cut to the
 * connector's DRM_MAX_MODES: an EDID with a range descriptor and a long
 * CTA block yields a few hundred. */
#define DRM_PROBE_MAX_MODES 256

/* The full probe of one connector: detect, get_modes (with the standard
 * modes up to 1024x768 when the driver found none on a connected sink, and
 * 640x480 as the last resort on DisplayPort), validation against maxX x
 * maxY (0: no limit) and the checks above, pruning, sorting.  A
 * disconnected connector loses its EDID and its modes.  Returns the
 * number of modes found (before validation), as the driver reported. */
int drm_helper_probe_single_connector_modes(struct drm_device *dev,
					    struct drm_connector *c,
					    uint32_t maxX, uint32_t maxY);

/* Ask the driver whether a sink is connected: DRM_MODE_CONNECTED /
 * DISCONNECTED / UNKNOWNCONNECTION (connected when the driver cannot
 * tell).  Bumps the connector's epoch counter when that differs from what
 * the connector last said.  Does not change the connector itself. */
int drm_helper_probe_detect(struct drm_device *dev, struct drm_connector *c,
			    bool force);

/* Run every mode of the connector's list through the core's checks and
 * the driver's, drop the refused ones and the duplicates, and sort the
 * rest best first.  Returns the number left. */
int drm_helper_probe_finish_modes(struct drm_device *dev,
				  struct drm_connector *c);

/* The verdict of the core's and the driver's checks on one mode for this
 * connector (what the probe applies to each mode, with the device's size
 * limits): MODE_OK or why not. */
enum drm_mode_status drm_connector_mode_valid(struct drm_device *dev,
					      struct drm_connector *c,
					      const struct drm_display_mode *mode);

/* The list steps on an array of modes the caller owns (the connector code
 * uses them on an EDID's modes): equal modes merged, every mode checked
 * against maxX x maxY (0: no limit) and the checks above, the refused ones
 * dropped, the rest sorted best first.  Returns the number left. */
int drm_helper_probe_validate_list(struct drm_device *dev,
				   struct drm_connector *c,
				   struct drm_display_mode *modes, int n,
				   uint32_t maxX, uint32_t maxY);
/* Replace the connector's list with the first DRM_MAX_MODES of `modes',
 * in the form clients see. */
void drm_helper_probe_store_modes(struct drm_connector *c,
				  const struct drm_display_mode *modes, int n);
/* The connector's list from an EDID's modes (the n in `modes', which the
 * call reorders): validate_list with the device's size limits, then one
 * preferred mode -- the one `summary' (drm_edid_parse of the same EDID)
 * chose, or the best left when that was refused -- at modes[0].  Returns
 * the number of modes the connector now has. */
struct drm_edid_info;
int drm_helper_probe_edid_list(struct drm_device *dev, struct drm_connector *c,
			       struct drm_display_mode *modes, int n,
			       const struct drm_edid_info *summary);

/* Tell clients that connectors changed: the device's hotplug count goes
 * up (clients poll it in /sys and re-read their connectors). */
void drm_kms_helper_hotplug_event(struct drm_device *dev);
void drm_kms_helper_connector_hotplug_event(struct drm_connector *c);

/* A driver's hotplug interrupt: detect again and, when the connector's
 * epoch moved (status or EDID changed), send the hotplug event.  True when
 * something changed.  Process context: the driver's detect may sleep. */
bool drm_connector_helper_hpd_irq_event(struct drm_device *dev,
					struct drm_connector *c);
/* The same for every connector of the device. */
bool drm_helper_hpd_irq_event(struct drm_device *dev);

/* For a panel with one native timing: refuse a mode of another size. */
enum drm_mode_status drm_crtc_helper_mode_valid_fixed(const struct drm_display_mode *mode,
						      const struct drm_display_mode *fixed_mode);
/* get_modes for such a panel: the fixed mode, preferred, and the panel's
 * physical size when the mode carries one.  1, or 0 when the list is
 * full. */
int drm_connector_helper_get_modes_fixed(struct drm_device *dev,
					 struct drm_connector *c,
					 const struct drm_display_mode *fixed_mode);

#endif
