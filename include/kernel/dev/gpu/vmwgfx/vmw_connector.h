// LikeOS -- vmwgfx: the connectors of the display units.
//
// A display unit's connector has no sink to ask.  Whether it is connected,
// what it prefers and where it sits are what the host's layout says
// (DRM_VMW_UPDATE_LAYOUT, recorded in struct vmw_display_unit as
// pref_active, pref_w x pref_h and gui_x/gui_y); what it can show is what
// the scan-out path can carry.  The DRM core's probe asks this file:
//   - detect: connected while the layout makes the unit part of the host's
//     desktop (and the device has that many displays);
//   - get_modes: the preferred size first, then the size the unit came up
//     with and the mode the boot console left, then the standard sizes the
//     scan-out can carry, largest first;
//   - mode_valid: the scan-out's geometry and memory limits.
// The core then checks, de-duplicates and sorts the list
// (DRM_FEATURE_PROBE_HELPER), preferred mode first.
//
// Each connector also carries "suggested X" / "suggested Y" (the unit's
// place in the host's desktop), "hotplug_mode_update" (1: a hot-plug event
// from this device means "apply the preferred mode", which desktop
// environments act on) and, on the legacy display, "implicit_placement"
// (1: the units are placed by the device, not by the client).  All are
// immutable: the host's layout changes them, a client only reads them.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_CONNECTOR_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_CONNECTOR_H

#include <kernel/dev/gpu/drm.h>

struct vmw_device;

/* The core probes the connectors through the hooks below.
 *
 * 1: vmw_connector_driver_setup() installs detect / get_modes / mode_valid
 * and DRM_FEATURE_PROBE_HELPER: a client's GETCONNECTOR and every host
 * layout re-probe the connector, and the list it gets is checked against
 * the scan-out, merged and sorted by the core, the preferred mode first.
 * A unit the layout leaves out is disconnected and has no modes.
 *
 * 0: no hooks.  Each connector keeps the list built when the driver came
 * up, in that order, and a host layout only moves the preferred mark
 * within it (adding the size when it is new); a unit the layout leaves out
 * keeps its list. */
#ifndef VMW_CONNECTOR_PROBE
#define VMW_CONNECTOR_PROBE 1
#endif

/* The connector properties "suggested X", "suggested Y",
 * "hotplug_mode_update" and (legacy display only) "implicit_placement".
 *
 * 1: created and attached by vmw_connector_init(), the suggested offsets
 * kept up to date by vmw_connector_layout_changed().
 * 0: none; connectors list only the core's properties. */
#ifndef VMW_CONNECTOR_PROPS
#define VMW_CONNECTOR_PROPS 1
#endif

/* Whether detect also asks the device how many displays it has
 * (SVGA_REG_NUM_DISPLAYS): a unit at or beyond that count is disconnected
 * whatever the layout says.  A device that answers 0 does not implement
 * the register and sets no bound.
 *
 * 1: asked.  0: the layout alone decides. */
#ifndef VMW_CONNECTOR_NUM_DISPLAYS
#define VMW_CONNECTOR_NUM_DISPLAYS 1
#endif

/* ---- drm_driver hooks ---- */

/* DRM_MODE_CONNECTED while the unit is part of the host's desktop, else
 * DRM_MODE_DISCONNECTED. */
int vmw_connector_detect(struct drm_device *dev, struct drm_connector *c);
/* Refill the connector's list (see the top of this file); the number of
 * modes it now has. */
int vmw_connector_get_modes(struct drm_device *dev, struct drm_connector *c);
/* MODE_OK when the unit's scan-out can show a mode of this size:
 * MODE_VIRTUAL_X / _Y beyond its largest width / height, MODE_MEM beyond
 * its memory at 32 bits per pixel, MODE_BAD for a connector that is not
 * a unit. */
enum drm_mode_status vmw_connector_mode_valid(struct drm_device *dev,
					      struct drm_connector *c,
					      const struct drm_display_mode *mode);

/* Install the hooks above and DRM_FEATURE_PROBE_HELPER in a driver table
 * (with VMW_CONNECTOR_PROBE; nothing otherwise).  Before
 * drm_dev_register(). */
void vmw_connector_driver_setup(struct drm_driver *drv);

/* ---- set-up ---- */

/* Unit `unit''s connector exists and has the mode list the driver built
 * for it (vmwgfx_init() for the first, vmw_kms_init_display() for the
 * others).  Records what that list was made from, so that get_modes gives
 * the same list until the host's layout changes something: the preferred
 * size on it becomes the unit's pref_w x pref_h, and for the first unit
 * the mode the device is in now is the boot mode.  Creates the connector
 * properties (once per device) and attaches them.
 *
 * Call after vmw_kms_init_display() and BEFORE the console takes the
 * display (drm_console_takeover()), which may change the device's mode.
 * 0, -EINVAL for a unit that does not exist, -ENOMEM when the properties
 * could not be created (the connector then works without them). */
int vmw_connector_init(struct vmw_device *v, int unit);
/* The same for every unit (vmw_du_count()); the first error. */
int vmw_connector_init_all(struct vmw_device *v);

/* ---- the host's layout ---- */

/* The units' pref_active / pref_w / pref_h / gui_x / gui_y were just set
 * from a host layout (DRM_VMW_UPDATE_LAYOUT).  Updates every connector:
 * its suggested offsets, its status and mode list (a re-probe through the
 * core with the hooks installed; with VMW_CONNECTOR_PROBE 0 the preferred
 * mark moved in place), and tells clients (the device's hot-plug count).
 * Process context, without the locks the probe hooks take (they take
 * none).  0, or -ENOSPC when the first unit's list had no room for its new
 * preferred size (only without the hooks). */
int vmw_connector_layout_changed(struct vmw_device *v);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_CONNECTOR_H */
