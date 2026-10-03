// LikeOS -- vmwgfx: the connectors of the display units.
//
// What the DRM core's probe of a unit's connector finds (detect, get_modes,
// mode_valid), the connector properties that tell a desktop where the host
// puts each screen ("suggested X" / "suggested Y", "hotplug_mode_update",
// "implicit_placement"), and what a new host layout does to all of them.
// The layout itself -- which units are part of the host's desktop, at what
// size and where -- is recorded in struct vmw_display_unit by the
// DRM_VMW_UPDATE_LAYOUT handler (vmw_kms.c); this file only reads it.
//
// The mode list a unit offers is built from a few sizes, in this order:
//   1. the size the host's layout prefers (pref_w x pref_h), marked
//      preferred -- before any layout, the size the unit came up with;
//   2. the size the unit came up with, when a layout has moved the
//      preference away from it, so that going back needs no new mode;
//   3. on the first unit, the mode the boot console left the device in,
//      so that falling back to it needs no mode set the device could
//      refuse;
//   4. the standard sizes, largest first, that the scan-out can carry.
// Until a layout arrives that is exactly the list the driver built when it
// came up.  The probe then checks every mode against the scan-out
// (vmw_connector_mode_valid()), merges equal ones and sorts the list, the
// preferred mode first -- which is where the console and the display
// servers look.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_probe_helper.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/uapi/bug.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/vmwgfx/vmw_connector.h>

/* ---- the standard sizes --------------------------------------------------- */

/* Largest first.  The first unit offers the entries without `further_only'
 * -- the boot console's own table (kernel/dev/video/vmsvga2.c), so the
 * console and a display server agree about what the first screen can be;
 * the further units, which the console never shows, offer all of them. */
static const struct {
	uint16_t w, h;
	uint8_t further_only;
} vmw_connector_builtin_modes[] = {
	{ 3840, 2160, 1 }, { 2560, 1600, 0 }, { 2560, 1440, 0 },
	{ 2048, 1152, 1 }, { 1920, 1440, 0 }, { 1920, 1200, 0 },
	{ 1920, 1080, 0 }, { 1856, 1392, 0 }, { 1792, 1344, 0 },
	{ 1680, 1050, 0 }, { 1600, 1200, 0 }, { 1600, 900, 0 },
	{ 1440, 900, 0 },  { 1400, 1050, 0 }, { 1366, 768, 0 },
	{ 1360, 768, 0 },  { 1280, 1024, 0 }, { 1280, 960, 0 },
	{ 1280, 800, 0 },  { 1280, 768, 0 },  { 1280, 720, 0 },
	{ 1152, 864, 0 },  { 1024, 768, 0 },  { 800, 600, 0 },
	{ 640, 480, 0 },
};

/* ---- per-device state ------------------------------------------------------ */

/* What a unit's list is made of besides the layout's preference. */
struct vmw_connector_unit {
	int ready;		/* vmw_connector_init() has run */
	uint32_t init_w, init_h;	/* the size it came up with */
	uint32_t boot_w, boot_h;	/* the boot console's mode, 0: none */
};

/* The device has one instance of each property, attached to every
 * connector that carries it.  There is one SVGA device; `dev' says which
 * device this state belongs to. */
static struct {
	struct drm_device *dev;
	struct drm_prop *suggested_x;
	struct drm_prop *suggested_y;
	struct drm_prop *hotplug_mode_update;
	struct drm_prop *implicit_placement;
	struct vmw_connector_unit unit[VMW_MAX_HEADS];
} g_vmw_conn;

static void vmw_connector_state_bind(struct vmw_device *v)
{
	if (g_vmw_conn.dev == &v->drm)
		return;
	mm_memset(&g_vmw_conn, 0, sizeof(g_vmw_conn));
	g_vmw_conn.dev = &v->drm;
}

/* The unit of a connector of this device, NULL when it is not one. */
static struct vmw_display_unit *vmw_connector_du(struct vmw_device *v,
						 const struct drm_connector *c,
						 int *index)
{
	if (c < v->drm.conn || c >= v->drm.conn + v->drm.nconn)
		return NULL;
	*index = (int)(c - v->drm.conn);
	return vmw_du(v, *index);
}

/* ---- detect ----------------------------------------------------------------- */

int vmw_connector_detect(struct drm_device *dev, struct drm_connector *c)
{
	struct vmw_device *v = dev->priv;
	struct vmw_display_unit *du;
	int unit;

	du = vmw_connector_du(v, c, &unit);
	if (!du)
		return DRM_MODE_DISCONNECTED;
	if (VMW_CONNECTOR_NUM_DISPLAYS) {
		/* The displays the device has.  A zero is a device without
		 * the register, not a device without displays. */
		uint32_t num_displays = vmsvga2_hw_read_reg(SVGA_REG_NUM_DISPLAYS);

		if (num_displays && (uint32_t)unit >= num_displays)
			return DRM_MODE_DISCONNECTED;
	}
	return du->pref_active ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
}

/* ---- modes ------------------------------------------------------------------ */

/* Can the unit's scan-out carry a w x h screen at 32 bits per pixel?  The
 * bounds are the ones the scan-out path in use states (vmw_device:
 * scanout_max_width / _height / _mem, see vmw_scanout_limits()), not the
 * framebuffer aperture's unless that is the path. */
static int vmw_connector_fits(const struct vmw_device *v, uint32_t w,
			      uint32_t h)
{
	return w != 0 && h != 0 && w <= v->scanout_max_width &&
	       h <= v->scanout_max_height &&
	       (uint64_t)w * h * 4 <= v->scanout_max_mem;
}

static int vmw_connector_add(struct vmw_device *v, int unit, uint32_t w,
			     uint32_t h, int preferred)
{
	struct drm_mode_modeinfo m;

	drm_mode_fill(&m, w, h, 60, preferred);
	return drm_connector_add_mode(&v->drm, unit, &m);
}

static int same_size(uint32_t w, uint32_t h, uint32_t w2, uint32_t h2)
{
	return w == w2 && h == h2;
}

int vmw_connector_get_modes(struct drm_device *dev, struct drm_connector *c)
{
	struct vmw_device *v = dev->priv;
	struct vmw_display_unit *du;
	const struct vmw_connector_unit *u;
	uint32_t pref_w, pref_h, init_w = 0, init_h = 0, boot_w = 0, boot_h = 0;
	int unit;

	du = vmw_connector_du(v, c, &unit);
	if (!du) {
		c->nmodes = 0;
		return 0;
	}
	/* One look at the layout: it may change under a probe, and the list
	 * should come from one version of it. */
	pref_w = du->pref_w;
	pref_h = du->pref_h;
	if (!pref_w || !pref_h)
		vmw_kms_initial_size(v, &pref_w, &pref_h);
	if (g_vmw_conn.dev == dev) {
		u = &g_vmw_conn.unit[unit];
		if (u->ready) {
			init_w = u->init_w;
			init_h = u->init_h;
			boot_w = u->boot_w;
			boot_h = u->boot_h;
		}
	}

	drm_connector_clear_modes(dev, unit);

	/* The layout's preference, unchecked: the probe drops it when the
	 * scan-out cannot carry it, and without the probe it has always been
	 * offered as the host asked. */
	vmw_connector_add(v, unit, pref_w, pref_h, 1);
	if (init_w && !same_size(init_w, init_h, pref_w, pref_h))
		vmw_connector_add(v, unit, init_w, init_h, 0);
	if (boot_w && boot_h && !same_size(boot_w, boot_h, pref_w, pref_h) &&
	    !same_size(boot_w, boot_h, init_w, init_h))
		vmw_connector_add(v, unit, boot_w, boot_h, 0);

	for (unsigned i = 0; i < ARRAY_SIZE(vmw_connector_builtin_modes); i++) {
		uint32_t w = vmw_connector_builtin_modes[i].w;
		uint32_t h = vmw_connector_builtin_modes[i].h;

		if (unit == 0 && vmw_connector_builtin_modes[i].further_only)
			continue;
		if (same_size(w, h, pref_w, pref_h) ||
		    same_size(w, h, init_w, init_h) ||
		    same_size(w, h, boot_w, boot_h))
			continue;
		if (!vmw_connector_fits(v, w, h))
			continue;
		if (vmw_connector_add(v, unit, w, h, 0) != 0)
			break; /* the list is full */
	}
	return (int)c->nmodes;
}

enum drm_mode_status vmw_connector_mode_valid(struct drm_device *dev,
					      struct drm_connector *c,
					      const struct drm_display_mode *mode)
{
	struct vmw_device *v = dev->priv;
	enum drm_mode_status ret;
	int unit;

	if (!vmw_connector_du(v, c, &unit))
		return MODE_BAD;

	ret = drm_mode_validate_size(mode, (int)v->scanout_max_width,
				     (int)v->scanout_max_height);
	if (ret != MODE_OK)
		return ret;

	/* The device's primary limits are always in 32 bits per pixel. */
	if ((uint64_t)mode->hdisplay * 4 * mode->vdisplay > v->scanout_max_mem)
		return MODE_MEM;

	return MODE_OK;
}

void vmw_connector_driver_setup(struct drm_driver *drv)
{
	if (!VMW_CONNECTOR_PROBE)
		return;
	drv->detect = vmw_connector_detect;
	drv->get_modes = vmw_connector_get_modes;
	drv->mode_valid = vmw_connector_mode_valid;
	drv->features |= DRM_FEATURE_PROBE_HELPER;
}

/* Are the hooks above what the device's driver table uses? */
static int vmw_connector_hooks_installed(const struct vmw_device *v)
{
	return v->drm.drv && v->drm.drv->get_modes == vmw_connector_get_modes;
}

/* ---- properties --------------------------------------------------------------- */

/* The device's instances, made once.  "implicit_placement" only where the
 * legacy display is the path: there the device places the one screen, and
 * a client is told it cannot. */
static int vmw_connector_create_properties(struct vmw_device *v)
{
	struct drm_device *dev = &v->drm;

	if (!g_vmw_conn.suggested_x)
		g_vmw_conn.suggested_x = drm_property_create_range(
			dev, DRM_MODE_PROP_IMMUTABLE, "suggested X", 0,
			0xffffffff);
	if (!g_vmw_conn.suggested_y)
		g_vmw_conn.suggested_y = drm_property_create_range(
			dev, DRM_MODE_PROP_IMMUTABLE, "suggested Y", 0,
			0xffffffff);
	if (!g_vmw_conn.hotplug_mode_update)
		g_vmw_conn.hotplug_mode_update = drm_property_create_range(
			dev, DRM_MODE_PROP_IMMUTABLE, "hotplug_mode_update", 0,
			1);
	if (!g_vmw_conn.implicit_placement &&
	    vmw_active_display_unit(v) == vmw_du_legacy)
		g_vmw_conn.implicit_placement = drm_property_create_range(
			dev, DRM_MODE_PROP_IMMUTABLE, "implicit_placement", 0,
			1);

	if (!g_vmw_conn.suggested_x || !g_vmw_conn.suggested_y ||
	    !g_vmw_conn.hotplug_mode_update)
		return -ENOMEM;
	return 0;
}

/* A desktop position as the property holds it: never negative. */
static uint64_t vmw_connector_offset(int32_t pos)
{
	return pos > 0 ? (uint64_t)pos : 0;
}

static void vmw_connector_attach_properties(struct vmw_device *v,
					    struct drm_connector *c,
					    const struct vmw_display_unit *du)
{
	if (g_vmw_conn.hotplug_mode_update)
		drm_object_attach_property(&c->properties,
					   g_vmw_conn.hotplug_mode_update, 1);
	if (g_vmw_conn.suggested_x)
		drm_object_attach_property(&c->properties,
					   g_vmw_conn.suggested_x,
					   vmw_connector_offset(du->gui_x));
	if (g_vmw_conn.suggested_y)
		drm_object_attach_property(&c->properties,
					   g_vmw_conn.suggested_y,
					   vmw_connector_offset(du->gui_y));
	if (g_vmw_conn.implicit_placement &&
	    vmw_active_display_unit(v) == vmw_du_legacy)
		drm_object_attach_property(&c->properties,
					   g_vmw_conn.implicit_placement, 1);
}

/* ---- set-up ---------------------------------------------------------------------- */

int vmw_connector_init(struct vmw_device *v, int unit)
{
	struct vmw_display_unit *du = vmw_du(v, unit);
	struct vmw_connector_unit *u;
	struct drm_connector *c;
	uint32_t w, h;
	int rc = 0;

	if (!du || (uint32_t)unit >= v->drm.nconn)
		return -EINVAL;
	vmw_connector_state_bind(v);
	c = &v->drm.conn[unit];
	u = &g_vmw_conn.unit[unit];

	/* The size the unit came up with is the one its list marks
	 * preferred: for the first unit the driver weighed the host's
	 * recommendation and the build's screen-size preference to pick it,
	 * and that choice stays the unit's preference until the host's
	 * layout says otherwise.  A list without a mark: its first mode,
	 * which is what the console took. */
	w = 0;
	h = 0;
	for (uint32_t i = 0; i < c->nmodes; i++) {
		if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			w = c->modes[i].hdisplay;
			h = c->modes[i].vdisplay;
			break;
		}
	}
	if (!(w && h) && c->nmodes) {
		w = c->modes[0].hdisplay;
		h = c->modes[0].vdisplay;
	}
	/* No list at all: what the layout code set up. */
	if (!(w && h)) {
		w = du->pref_w;
		h = du->pref_h;
	}
	if (!(w && h))
		vmw_kms_initial_size(v, &w, &h);
	du->pref_w = w;
	du->pref_h = h;
	u->init_w = w;
	u->init_h = h;

	/* The boot console's mode: what the device is in until the console
	 * takes the display -- hence the call before that. */
	if (unit == 0) {
		u->boot_w = v->hw.width;
		u->boot_h = v->hw.height;
	} else {
		u->boot_w = 0;
		u->boot_h = 0;
	}
	u->ready = 1;

	if (VMW_CONNECTOR_PROPS) {
		rc = vmw_connector_create_properties(v);
		vmw_connector_attach_properties(v, c, du);
	}
	return rc;
}

int vmw_connector_init_all(struct vmw_device *v)
{
	int rc = 0;

	for (int i = 0; i < vmw_du_count(v); i++) {
		int r = vmw_connector_init(v, i);

		if (r && !rc)
			rc = r;
	}
	return rc;
}

/* ---- the host's layout ---------------------------------------------------------- */

/* Without the probe hooks: make w x h the one preferred mode of the unit's
 * list, replacing an entry of the same size or adding one. */
static int vmw_connector_mark_preferred(struct vmw_device *v, int unit,
					uint32_t w, uint32_t h)
{
	struct drm_connector *c = &v->drm.conn[unit];
	struct drm_mode_modeinfo m;

	drm_mode_fill(&m, w, h, 60, 1);
	/* Preferred is a property of exactly one mode. */
	for (uint32_t i = 0; i < c->nmodes; i++)
		c->modes[i].type &= (uint32_t)~DRM_MODE_TYPE_PREFERRED;
	for (uint32_t i = 0; i < c->nmodes; i++) {
		if (c->modes[i].hdisplay == w && c->modes[i].vdisplay == h) {
			c->modes[i] = m;
			return 0;
		}
	}
	if (drm_connector_add_mode(&v->drm, unit, &m) != 0)
		return -ENOSPC;
	return 0;
}

int vmw_connector_layout_changed(struct vmw_device *v)
{
	int nunits = vmw_du_count(v);
	int probe = vmw_connector_hooks_installed(v);
	int rc = 0;

	for (int i = 0; i < nunits; i++) {
		struct vmw_display_unit *du = vmw_du(v, i);
		struct drm_connector *c = &v->drm.conn[i];

		/* Where the host puts the unit; a unit outside its desktop
		 * is at the origin. */
		if (g_vmw_conn.dev == &v->drm) {
			if (g_vmw_conn.suggested_x)
				drm_object_property_set_value(
					&c->properties, g_vmw_conn.suggested_x,
					du->pref_active ?
						vmw_connector_offset(du->gui_x) :
						0);
			if (g_vmw_conn.suggested_y)
				drm_object_property_set_value(
					&c->properties, g_vmw_conn.suggested_y,
					du->pref_active ?
						vmw_connector_offset(du->gui_y) :
						0);
		}

		if (probe) {
			/* Status and list from a probe, and the hot-plug
			 * count bumped for the clients to look again. */
			drm_connector_hotplug(&v->drm, i);
			continue;
		}

		/* No hooks: the list stays, the preference moves in it, and
		 * only a change of status is a hot-plug. */
		int urc = vmw_connector_mark_preferred(v, i, du->pref_w,
						       du->pref_h);

		if (i == 0 && urc)
			rc = urc;
		if (c->connected != du->pref_active) {
			c->connected = du->pref_active;
			drm_connector_hotplug(&v->drm, i);
		}
	}
	return rc;
}
