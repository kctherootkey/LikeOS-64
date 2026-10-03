// LikeOS -- display-manager core: connectors and encoders.
//
// A connector is an output a sink plugs into; adding one also makes its
// encoder and its crtc (connector i drives encoder i drives crtc i) with
// that crtc's primary and cursor planes, and attaches the core's
// properties to each of them.  The connector's mode list comes from the
// driver -- an EDID through drm_connector_set_edid(), plus the standard
// table filtered by the sink's range through drm_connector_add_std_modes()
// -- and is re-probed on GETCONNECTOR and on a hotplug (drm_probe_helper.c
// for a driver that wants the whole probe done by the core).
//
// An EDID fills in what the sink is (display_info: size, colour depth and
// formats, HDMI capabilities, refresh range; the ELD for audio; a tile of a
// multi-connector monitor), replaces the EDID blob property, and gives the
// mode list: every mode the EDID lists, checked by the core and the driver,
// equal ones merged, refused ones dropped, sorted best first.  The preferred
// mode goes first because the console and the drivers take modes[0].
//
// The optional connector properties ("max bpc", "Colorspace", "Broadcast
// RGB", "content type", HDR_OUTPUT_METADATA, "vrr_capable", "scaling mode",
// "panel orientation", PATH, TILE) exist for drivers that attach them; a
// driver that attaches none lists exactly the core's.
//
// The connector and encoder ioctls: GETCONNECTOR (modes filtered for what
// the client said it understands: stereo layouts, picture aspect ratios),
// GETENCODER and the connector SETPROPERTY.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Intel's code: HPND-sell-variant
// Portions Copyright (C) 2016 Intel Corporation
// SPDX-License-Identifier for the portions derived from Luc Verhaegen's, Intel's and Red Hat's code: MIT
// Portions Copyright (C) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2010 Red Hat, Inc.

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_probe_helper.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* How drm_connector_set_edid() and drm_connector_add_std_modes() build the
 * mode list.
 *
 * 1: every mode the EDID lists (detailed, CVT, standard, established,
 * CTA-861, DisplayID, range-inferred), checked by the core (sane timings,
 * the device's size limits, interlace / double scan / stereo / 4:2:0-only
 * as the connector allows) and by the driver's mode_valid, equal modes
 * merged, sorted preferred first; the standard table's modes checked the
 * same way.  The EDID's preferred mode stays the one preferred mode.
 *
 * 0: the earlier list: the EDID's modes as the fixed-size summary
 * (drm_edid_parse) gives them, preferred first then in EDID order, and the
 * standard modes unchecked. */
#define DRM_CONNECTOR_PROBE_HELPER 1

/* When the device-wide connector properties that only some drivers use
 * (PATH, TILE, HDR_OUTPUT_METADATA) are created.
 *
 * 0: the first time a driver attaches one.  A driver that never does
 * gets no new properties, so the ids of everything created after the
 * core's properties stay what they were.
 * 1: all of them by drm_connector_create_standard_properties() at device
 * set-up, whether a driver uses them or not. */
#define DRM_CONNECTOR_STD_PROPS_EARLY 0

/* ---- lookups ------------------------------------------------------------ */

struct drm_connector *drm_conn_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nconn; i++)
		if (dev->conn[i].id == id)
			return &dev->conn[i];
	return NULL;
}

struct drm_encoder *drm_enc_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nenc; i++)
		if (dev->enc[i].id == id)
			return &dev->enc[i];
	return NULL;
}

/* ---- names -------------------------------------------------------------- */

static const struct drm_prop_enum_list drm_connector_enum_list[] = {
	{ DRM_MODE_CONNECTOR_Unknown, "Unknown" },
	{ DRM_MODE_CONNECTOR_VGA, "VGA" },
	{ DRM_MODE_CONNECTOR_DVII, "DVI-I" },
	{ DRM_MODE_CONNECTOR_DVID, "DVI-D" },
	{ DRM_MODE_CONNECTOR_DVIA, "DVI-A" },
	{ DRM_MODE_CONNECTOR_Composite, "Composite" },
	{ DRM_MODE_CONNECTOR_SVIDEO, "SVIDEO" },
	{ DRM_MODE_CONNECTOR_LVDS, "LVDS" },
	{ DRM_MODE_CONNECTOR_Component, "Component" },
	{ DRM_MODE_CONNECTOR_9PinDIN, "DIN" },
	{ DRM_MODE_CONNECTOR_DisplayPort, "DP" },
	{ DRM_MODE_CONNECTOR_HDMIA, "HDMI-A" },
	{ DRM_MODE_CONNECTOR_HDMIB, "HDMI-B" },
	{ DRM_MODE_CONNECTOR_TV, "TV" },
	{ DRM_MODE_CONNECTOR_eDP, "eDP" },
	{ DRM_MODE_CONNECTOR_VIRTUAL, "Virtual" },
	{ DRM_MODE_CONNECTOR_DSI, "DSI" },
	{ DRM_MODE_CONNECTOR_DPI, "DPI" },
	{ DRM_MODE_CONNECTOR_WRITEBACK, "Writeback" },
	{ DRM_MODE_CONNECTOR_SPI, "SPI" },
	{ DRM_MODE_CONNECTOR_USB, "USB" },
};

const char *drm_get_connector_type_name(unsigned int type)
{
	if (type < ARRAY_SIZE(drm_connector_enum_list))
		return drm_connector_enum_list[type].name;

	return NULL;
}

const char *drm_get_connector_status_name(int status)
{
	if (status == DRM_MODE_CONNECTED)
		return "connected";
	else if (status == DRM_MODE_DISCONNECTED)
		return "disconnected";
	else
		return "unknown";
}

static const struct drm_prop_enum_list drm_subpixel_enum_list[] = {
	{ SubPixelUnknown, "Unknown" },
	{ SubPixelHorizontalRGB, "Horizontal RGB" },
	{ SubPixelHorizontalBGR, "Horizontal BGR" },
	{ SubPixelVerticalRGB, "Vertical RGB" },
	{ SubPixelVerticalBGR, "Vertical BGR" },
	{ SubPixelNone, "None" },
};

const char *drm_get_subpixel_order_name(int order)
{
	if (order < 0 || order >= (int)ARRAY_SIZE(drm_subpixel_enum_list))
		return NULL;
	return drm_subpixel_enum_list[order].name;
}

static const struct drm_prop_enum_list drm_dpms_enum_list[] = {
	{ DRM_MODE_DPMS_ON, "On" },
	{ DRM_MODE_DPMS_STANDBY, "Standby" },
	{ DRM_MODE_DPMS_SUSPEND, "Suspend" },
	{ DRM_MODE_DPMS_OFF, "Off" }
};

const char *drm_get_dpms_name(int val)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(drm_dpms_enum_list); i++)
		if (drm_dpms_enum_list[i].type == val)
			return drm_dpms_enum_list[i].name;
	return "(unknown)";
}

/* Optional connector properties. */
static const struct drm_prop_enum_list drm_scaling_mode_enum_list[] = {
	{ DRM_MODE_SCALE_NONE, "None" },
	{ DRM_MODE_SCALE_FULLSCREEN, "Full" },
	{ DRM_MODE_SCALE_CENTER, "Center" },
	{ DRM_MODE_SCALE_ASPECT, "Full aspect" },
};

static const struct drm_prop_enum_list drm_content_type_enum_list[] = {
	{ DRM_MODE_CONTENT_TYPE_NO_DATA, "No Data" },
	{ DRM_MODE_CONTENT_TYPE_GRAPHICS, "Graphics" },
	{ DRM_MODE_CONTENT_TYPE_PHOTO, "Photo" },
	{ DRM_MODE_CONTENT_TYPE_CINEMA, "Cinema" },
	{ DRM_MODE_CONTENT_TYPE_GAME, "Game" },
};

static const struct drm_prop_enum_list drm_panel_orientation_enum_list[] = {
	{ DRM_MODE_PANEL_ORIENTATION_NORMAL, "Normal" },
	{ DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP, "Upside Down" },
	{ DRM_MODE_PANEL_ORIENTATION_LEFT_UP, "Left Side Up" },
	{ DRM_MODE_PANEL_ORIENTATION_RIGHT_UP, "Right Side Up" },
};

static const char *const colorspace_names[] = {
	/* For Default case, driver will set the colorspace */
	[DRM_MODE_COLORIMETRY_DEFAULT] = "Default",
	/* Standard Definition Colorimetry based on CEA 861 */
	[DRM_MODE_COLORIMETRY_SMPTE_170M_YCC] = "SMPTE_170M_YCC",
	[DRM_MODE_COLORIMETRY_BT709_YCC] = "BT709_YCC",
	/* Standard Definition Colorimetry based on IEC 61966-2-4 */
	[DRM_MODE_COLORIMETRY_XVYCC_601] = "XVYCC_601",
	/* High Definition Colorimetry based on IEC 61966-2-4 */
	[DRM_MODE_COLORIMETRY_XVYCC_709] = "XVYCC_709",
	/* Colorimetry based on IEC 61966-2-1/Amendment 1 */
	[DRM_MODE_COLORIMETRY_SYCC_601] = "SYCC_601",
	/* Colorimetry based on IEC 61966-2-5 [33] */
	[DRM_MODE_COLORIMETRY_OPYCC_601] = "opYCC_601",
	/* Colorimetry based on IEC 61966-2-5 */
	[DRM_MODE_COLORIMETRY_OPRGB] = "opRGB",
	/* Colorimetry based on ITU-R BT.2020 */
	[DRM_MODE_COLORIMETRY_BT2020_CYCC] = "BT2020_CYCC",
	/* Colorimetry based on ITU-R BT.2020 */
	[DRM_MODE_COLORIMETRY_BT2020_RGB] = "BT2020_RGB",
	/* Colorimetry based on ITU-R BT.2020 */
	[DRM_MODE_COLORIMETRY_BT2020_YCC] = "BT2020_YCC",
	/* Added as part of Additional Colorimetry Extension in 861.G */
	[DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65] = "DCI-P3_RGB_D65",
	[DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER] = "DCI-P3_RGB_Theater",
	[DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED] = "RGB_WIDE_FIXED",
	/* Colorimetry based on scRGB (IEC 61966-2-2) */
	[DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT] = "RGB_WIDE_FLOAT",
	[DRM_MODE_COLORIMETRY_BT601_YCC] = "BT601_YCC",
};

const char *drm_get_colorspace_name(enum drm_colorspace colorspace)
{
	if ((unsigned int)colorspace < ARRAY_SIZE(colorspace_names) &&
	    colorspace_names[colorspace])
		return colorspace_names[colorspace];
	else
		return "(null)";
}

static const uint32_t hdmi_colorspaces =
	BIT(DRM_MODE_COLORIMETRY_SMPTE_170M_YCC) |
	BIT(DRM_MODE_COLORIMETRY_BT709_YCC) |
	BIT(DRM_MODE_COLORIMETRY_XVYCC_601) |
	BIT(DRM_MODE_COLORIMETRY_XVYCC_709) |
	BIT(DRM_MODE_COLORIMETRY_SYCC_601) |
	BIT(DRM_MODE_COLORIMETRY_OPYCC_601) |
	BIT(DRM_MODE_COLORIMETRY_OPRGB) |
	BIT(DRM_MODE_COLORIMETRY_BT2020_CYCC) |
	BIT(DRM_MODE_COLORIMETRY_BT2020_RGB) |
	BIT(DRM_MODE_COLORIMETRY_BT2020_YCC) |
	BIT(DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65) |
	BIT(DRM_MODE_COLORIMETRY_DCI_P3_RGB_THEATER);

/* The colorimetries a DisplayPort VSC SDP can signal (DP 1.4a, table
 * 2-120). */
static const uint32_t dp_colorspaces =
	BIT(DRM_MODE_COLORIMETRY_RGB_WIDE_FIXED) |
	BIT(DRM_MODE_COLORIMETRY_RGB_WIDE_FLOAT) |
	BIT(DRM_MODE_COLORIMETRY_OPRGB) |
	BIT(DRM_MODE_COLORIMETRY_DCI_P3_RGB_D65) |
	BIT(DRM_MODE_COLORIMETRY_BT2020_RGB) |
	BIT(DRM_MODE_COLORIMETRY_BT601_YCC) |
	BIT(DRM_MODE_COLORIMETRY_BT709_YCC) |
	BIT(DRM_MODE_COLORIMETRY_XVYCC_601) |
	BIT(DRM_MODE_COLORIMETRY_XVYCC_709) |
	BIT(DRM_MODE_COLORIMETRY_SYCC_601) |
	BIT(DRM_MODE_COLORIMETRY_OPYCC_601) |
	BIT(DRM_MODE_COLORIMETRY_BT2020_CYCC) |
	BIT(DRM_MODE_COLORIMETRY_BT2020_YCC);

static const struct drm_prop_enum_list broadcast_rgb_names[] = {
	{ DRM_HDMI_BROADCAST_RGB_AUTO, "Automatic" },
	{ DRM_HDMI_BROADCAST_RGB_FULL, "Full" },
	{ DRM_HDMI_BROADCAST_RGB_LIMITED, "Limited 16:235" },
};

const char *drm_hdmi_connector_get_broadcast_rgb_name(enum drm_hdmi_broadcast_rgb broadcast_rgb)
{
	if ((unsigned int)broadcast_rgb >= ARRAY_SIZE(broadcast_rgb_names))
		return NULL;

	return broadcast_rgb_names[broadcast_rgb].name;
}

/* ---- small helpers ---------------------------------------------------------- */

static bool str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static bool bytes_eq(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (size_t i = 0; i < n; i++)
		if (x[i] != y[i])
			return false;
	return true;
}

/* Has drm_kms_init made the core's properties?  Objects whose list is
 * looked at before that would never get them (see drm_mode_object.h). */
static bool core_props_ready(const struct drm_device *dev)
{
	return dev->prop_dpms != 0;
}

/* The property with this id when the connector carries it, else NULL. */
static struct drm_prop *conn_attached_prop(struct drm_connector *c, uint32_t prop_id)
{
	return drm_mode_obj_find_prop_id(c->dev, &c->properties, prop_id);
}

/* A device-wide property the connector code makes on demand, found by name
 * and type (the device keeps no id for it).  NULL when there is none. */
static struct drm_prop *dev_prop_named(struct drm_device *dev, const char *name,
				       uint32_t flags)
{
	for (uint32_t i = 0; i < dev->nprops; i++) {
		struct drm_prop *p = &dev->props[i];

		if (p->id && p->flags == flags && str_eq(p->name, name))
			return p;
	}
	return NULL;
}

/* An immutable blob property of the device (PATH, TILE), made on first
 * use. */
static struct drm_prop *dev_blob_prop(struct drm_device *dev, const char *name,
				      bool create)
{
	const uint32_t flags = DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE;
	struct drm_prop *p = dev_prop_named(dev, name, flags);

	if (!p && create)
		p = drm_property_create(dev, flags, name, 0);
	return p;
}

/* Record a value the connector's list reports for a property a client
 * cannot set from the state (link-status): the list is the value. */
static void conn_list_store(struct drm_connector *c, const struct drm_prop *prop,
			    uint64_t val)
{
	struct drm_object_properties *props = &c->properties;

	for (int i = 0; i < props->count; i++)
		if (props->ids[i] == prop->id)
			props->values[i] = val;
}

/* ---- the connector -------------------------------------------------------- */

void drm_mode_fill(struct drm_mode_modeinfo *m, uint32_t w, uint32_t h,
		   uint32_t hz, int preferred)
{
	mm_memset(m, 0, sizeof(*m));
	/* CVT-ish timings: the device does not care, but the fields must be
	 * consistent and give a plausible pixel clock. */
	uint32_t hbl = w / 5 + 64, vbl = 30;
	m->hdisplay = (uint16_t)w;
	m->hsync_start = (uint16_t)(w + hbl / 4);
	m->hsync_end = (uint16_t)(w + hbl / 2);
	m->htotal = (uint16_t)(w + hbl);
	m->vdisplay = (uint16_t)h;
	m->vsync_start = (uint16_t)(h + 3);
	m->vsync_end = (uint16_t)(h + 8);
	m->vtotal = (uint16_t)(h + vbl);
	m->vrefresh = hz;
	m->clock = (uint32_t)(((uint64_t)m->htotal * m->vtotal * hz) / 1000);
	m->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
	m->type = DRM_MODE_TYPE_DRIVER | (preferred ? DRM_MODE_TYPE_PREFERRED : 0);
	ksnprintf(m->name, sizeof(m->name), "%ux%u", w, h);
}

int drm_connector_add(struct drm_device *dev, uint32_t type, uint32_t mm_w,
		      uint32_t mm_h)
{
	if (dev->nconn >= DRM_MAX_CONNECTORS)
		return -1;
	int i = (int)dev->nconn++;
	struct drm_connector *c = &dev->conn[i];
	struct drm_encoder *e = &dev->enc[dev->nenc++];
	struct drm_crtc *cr = &dev->crtc[dev->ncrtc++];
	int pp, cp;

	mm_memset(c, 0, sizeof(*c));
	mm_memset(e, 0, sizeof(*e));
	mm_memset(cr, 0, sizeof(*cr));
	cr->id = drm_mode_id_alloc(dev);
	cr->index = i;
	/* its primary and cursor planes */
	pp = drm_plane_add(dev, DRM_PLANE_TYPE_PRIMARY, 1u << i, NULL, 0, NULL, 0);
	cp = drm_plane_add(dev, DRM_PLANE_TYPE_CURSOR, 1u << i, NULL, 0, NULL, 0);
	cr->primary_plane_id = pp >= 0 ? dev->planes[pp].id : drm_mode_id_alloc(dev);
	cr->cursor_plane_id = cp >= 0 ? dev->planes[cp].id : drm_mode_id_alloc(dev);
	for (int g = 0; g < 256; g++) {
		cr->gamma[0][g] = (uint16_t)(g << 8 | g);
		cr->gamma[1][g] = (uint16_t)(g << 8 | g);
		cr->gamma[2][g] = (uint16_t)(g << 8 | g);
	}
	cr->gamma_identity = 1;
	e->id = drm_mode_id_alloc(dev);
	e->type = DRM_MODE_ENCODER_VIRTUAL;
	e->possible_crtcs = 1u << i;
	c->id = drm_mode_id_alloc(dev);
	c->encoder_id = e->id;
	c->type = type;
	c->type_id = (uint32_t)i + 1;
	c->connected = 1;
	c->mm_width = mm_w;
	c->mm_height = mm_h;
	c->dpms = 0;
	c->dev = dev;
	/* What every driver here has always been offered; a driver that
	 * cannot send interlace or double scan, or can send stereo or
	 * YCbCr 4:2:0, says so after adding the connector. */
	c->interlace_allowed = true;
	c->doublescan_allowed = true;
	c->stereo_allowed = false;
	c->ycbcr_420_allowed = false;
	c->display_info.panel_orientation = DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
	c->display_info.source_physical_address = 0xffff;
	c->link_status = DRM_MODE_LINK_STATUS_GOOD;
	dev->vbl[i].period_ns = 1000000000ULL / (dev->refresh_hz ? dev->refresh_hz : 60);
	/* The core's properties, ahead of anything the driver attaches.
	 * Before drm_kms_init they do not exist yet; the first lookup
	 * attaches them then. */
	if (core_props_ready(dev)) {
		drm_connector_attach_core_properties(dev, c);
		drm_crtc_attach_core_properties(dev, cr);
		if (pp >= 0)
			drm_plane_attach_core_properties(dev, &dev->planes[pp]);
		if (cp >= 0)
			drm_plane_attach_core_properties(dev, &dev->planes[cp]);
	}
	return i;
}

int drm_connector_add_mode(struct drm_device *dev, int conn,
			   const struct drm_mode_modeinfo *m)
{
	struct drm_connector *c = &dev->conn[conn];
	if (c->nmodes >= DRM_MAX_MODES)
		return -1;
	c->modes[c->nmodes++] = *m;
	return 0;
}

void drm_connector_clear_modes(struct drm_device *dev, int conn)
{
	dev->conn[conn].nmodes = 0;
}

/* ---- tile groups ------------------------------------------------------------
 *
 * The connectors showing tiles of one monitor share a group, named by the
 * topology id the monitor's DisplayID block gives; the TILE property says
 * the group's number.  Groups are counted by the connectors in them. */

#define DRM_TILE_GROUPS 16

static struct {
	struct drm_device *dev;
	uint8_t topology_id[8];
	uint32_t id;
	int refs;
} g_tile_groups[DRM_TILE_GROUPS];
static uint32_t g_tile_group_next = 1;
static spinlock_t g_tile_lock = SPINLOCK_INIT("drm_tile");

/* The group of this topology (made when there is none), referenced; 0
 * when the table is full. */
static uint32_t tile_group_get(struct drm_device *dev, const uint8_t topology_id[8])
{
	int free_slot = -1;
	uint32_t id = 0;
	uint64_t fl;

	spin_lock_irqsave(&g_tile_lock, &fl);
	for (int i = 0; i < DRM_TILE_GROUPS; i++) {
		if (!g_tile_groups[i].refs) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (g_tile_groups[i].dev == dev &&
		    bytes_eq(g_tile_groups[i].topology_id, topology_id, 8)) {
			g_tile_groups[i].refs++;
			id = g_tile_groups[i].id;
			break;
		}
	}
	if (!id && free_slot >= 0) {
		g_tile_groups[free_slot].dev = dev;
		mm_memcpy(g_tile_groups[free_slot].topology_id, topology_id, 8);
		g_tile_groups[free_slot].id = g_tile_group_next++;
		g_tile_groups[free_slot].refs = 1;
		id = g_tile_groups[free_slot].id;
	}
	spin_unlock_irqrestore(&g_tile_lock, fl);
	return id;
}

static void tile_group_put(struct drm_device *dev, uint32_t id)
{
	uint64_t fl;

	if (!id)
		return;
	spin_lock_irqsave(&g_tile_lock, &fl);
	for (int i = 0; i < DRM_TILE_GROUPS; i++) {
		if (g_tile_groups[i].refs && g_tile_groups[i].dev == dev &&
		    g_tile_groups[i].id == id) {
			g_tile_groups[i].refs--;
			break;
		}
	}
	spin_unlock_irqrestore(&g_tile_lock, fl);
}

/* The connector's tile from what the EDID code found (has_tile false: none). */
static void update_tile_info(struct drm_connector *c, const struct drm_edid_tile *t)
{
	c->has_tile = t->has_tile;
	if (t->has_tile) {
		uint32_t tg = tile_group_get(c->dev, t->topology_id);

		c->tile_is_single_monitor = t->tile_is_single_monitor;
		c->num_h_tile = t->num_h_tile;
		c->num_v_tile = t->num_v_tile;
		c->tile_h_loc = t->tile_h_loc;
		c->tile_v_loc = t->tile_v_loc;
		c->tile_h_size = t->tile_h_size;
		c->tile_v_size = t->tile_v_size;
		if (tg && c->tile_group_id != tg) {
			/* a new group: drop the old one's reference */
			tile_group_put(c->dev, c->tile_group_id);
			c->tile_group_id = tg;
		} else {
			/* the same group: drop the reference just taken */
			tile_group_put(c->dev, tg);
		}
	}

	if (!c->has_tile && c->tile_group_id) {
		tile_group_put(c->dev, c->tile_group_id);
		c->tile_group_id = 0;
	}
}

/* ---- the EDID ----------------------------------------------------------------- */

/* Install the EDID (NULL: none) as the connector's EDID blob, and bring the
 * properties that follow it -- non-desktop, TILE -- up to date. */
static int connector_edid_property_update(struct drm_connector *c,
					  const void *edid, size_t size)
{
	struct drm_device *dev = c->dev;
	struct drm_prop *edid_prop;
	struct drm_prop *prop;
	int ret;

	if (!edid)
		size = 0;
	if (core_props_ready(dev))
		drm_connector_attach_core_properties(dev, c);

	if (c->edid_blob_ptr) {
		const struct drm_blob *old = c->edid_blob_ptr;

		if (old->length != size || !bytes_eq(old->data, edid, size))
			c->epoch_counter++;
	}

	edid_prop = conn_attached_prop(c, dev->prop_edid);
	ret = drm_property_replace_global_blob(dev, &c->edid_blob_ptr, size, edid,
					       edid_prop ? &c->properties : NULL,
					       edid_prop);
	c->edid_blob_id = c->edid_blob_ptr ? c->edid_blob_ptr->id : 0;
	if (ret)
		return ret;

	prop = conn_attached_prop(c, dev->prop_non_desktop);
	if (prop) {
		ret = drm_object_property_set_value(&c->properties, prop,
						    c->display_info.non_desktop);
		if (ret)
			return ret;
	}

	prop = dev_blob_prop(dev, "TILE", false);
	if (prop && conn_attached_prop(c, prop->id))
		ret = drm_connector_set_tile_property(c);
	return ret;
}

/* Forget the sink: no EDID, nothing known about it. */
static void connector_edid_remove(struct drm_connector *c)
{
	drm_display_info_reset(&c->display_info);
	mm_memset(c->eld, 0, sizeof(c->eld));
	mm_memset(c->latency_present, 0, sizeof(c->latency_present));
	mm_memset(c->video_latency, 0, sizeof(c->video_latency));
	mm_memset(c->audio_latency, 0, sizeof(c->audio_latency));
	c->has_tile = false;
	update_tile_info(c, &(struct drm_edid_tile){ 0 });
	(void)connector_edid_property_update(c, NULL, 0);
}

/* The fields the drivers read from the fixed-size summary. */
static void connector_summary_fields(struct drm_connector *c,
				     const struct drm_edid_info *info)
{
	if (info->mm_width && info->mm_height) {
		c->mm_width = info->mm_width;
		c->mm_height = info->mm_height;
	}
	if (info->has_range) {
		c->range_max_clock_khz = info->range.max_clock_khz;
		c->range_min_vrefresh = info->range.min_vrefresh;
		c->range_max_vrefresh = info->range.max_vrefresh;
		c->range_min_hfreq_khz = info->range.min_hfreq;
		c->range_max_hfreq_khz = info->range.max_hfreq;
	}
	c->is_hdmi = info->is_hdmi;
	mm_memcpy(c->sink_name, info->name, sizeof(c->sink_name));
}

/* The earlier list: the summary's modes, preferred first, the rest in EDID
 * order. */
static int set_edid_modes_summary(struct drm_device *dev, int conn,
				  const struct drm_edid_info *info)
{
	int n = 0;

	dev->conn[conn].nmodes = 0;
	if (info->preferred >= 0 && info->preferred < info->nmodes) {
		drm_connector_add_mode(dev, conn, &info->modes[info->preferred]);
		n++;
	}
	for (int i = 0; i < info->nmodes; i++) {
		if (i == info->preferred)
			continue;
		if (drm_connector_add_mode(dev, conn, &info->modes[i]) == 0)
			n++;
	}
	return n;
}

/* What an EDID parse needs besides the connector: the summary the drivers
 * read, and room for every mode the EDID lists. */
struct connector_edid_work {
	struct drm_edid_info info;
	struct drm_edid_conn ec;
	struct drm_display_mode modes[DRM_PROBE_MAX_MODES];
};

int drm_connector_set_edid(struct drm_device *dev, int conn,
			   const uint8_t *edid, unsigned len)
{
	struct drm_connector *c = &dev->conn[conn];
	struct connector_edid_work *w;
	const struct drm_edid *drm_edid;
	int n;

	c->range_max_clock_khz = 0;
	c->range_min_vrefresh = c->range_max_vrefresh = 0;
	c->range_min_hfreq_khz = c->range_max_hfreq_khz = 0;
	c->is_hdmi = 0;
	c->sink_name[0] = 0;
	if (!edid || len < DRM_EDID_BLOCK) {
		connector_edid_remove(c);
		return 0;
	}
	w = kalloc(sizeof(*w));
	if (!w) {
		connector_edid_remove(c);
		return -ENOMEM;
	}
	mm_memset(w, 0, sizeof(*w));
	if (drm_edid_parse(edid, len, &w->info) != 0) {
		kfree(w);
		connector_edid_remove(c);
		return -EINVAL;
	}
	/* The checked EDID: a damaged header repaired, extension blocks
	 * that fail dropped -- what the parse used, and what clients get. */
	drm_edid = drm_edid_alloc_checked(edid, len);
	if (!drm_edid) {
		kfree(w);
		connector_edid_remove(c);
		return -EINVAL;
	}

	/* What the sink is: display_info, its ELD, latencies, tile. */
	w->ec.connector_type = c->type;
	w->ec.info = &c->display_info;
	w->ec.eld = c->eld;
	w->ec.modes = w->modes;
	w->ec.max_modes = DRM_PROBE_MAX_MODES;
	drm_edid_update_display_info(&w->ec, drm_edid);
	mm_memcpy(c->latency_present, w->ec.latency_present, sizeof(c->latency_present));
	mm_memcpy(c->video_latency, w->ec.video_latency, sizeof(c->video_latency));
	mm_memcpy(c->audio_latency, w->ec.audio_latency, sizeof(c->audio_latency));
	update_tile_info(c, &w->ec.tile);
	connector_summary_fields(c, &w->info);
	(void)connector_edid_property_update(c, drm_edid->edid, drm_edid->size);

	if (DRM_CONNECTOR_PROBE_HELPER) {
		drm_edid_add_modes(&w->ec, drm_edid);
		n = drm_helper_probe_edid_list(dev, c, w->modes, w->ec.nmodes,
					       &w->info);
	} else {
		n = set_edid_modes_summary(dev, conn, &w->info);
	}

	drm_edid_free(drm_edid);
	kfree(w);
	return n;
}

/* ---- the standard table ---------------------------------------------------------- */

int drm_connector_add_std_modes(struct drm_device *dev, int conn,
				uint32_t max_clock_khz, uint32_t max_w,
				uint32_t max_h)
{
	struct drm_connector *c = &dev->conn[conn];
	struct drm_edid_range r;
	int added = 0;

	r.max_clock_khz = c->range_max_clock_khz;
	r.min_vrefresh = c->range_min_vrefresh;
	r.max_vrefresh = c->range_max_vrefresh;
	r.min_hfreq = c->range_min_hfreq_khz;
	r.max_hfreq = c->range_max_hfreq_khz;
	/* Without an EDID range, be conservative: nothing above the
	 * preferred/first mode's geometry and clock. */
	uint32_t lim_w = max_w, lim_h = max_h, lim_clk = max_clock_khz;
	if (!c->edid_blob_id && c->nmodes) {
		if (!lim_w || c->modes[0].hdisplay < lim_w)
			lim_w = c->modes[0].hdisplay;
		if (!lim_h || c->modes[0].vdisplay < lim_h)
			lim_h = c->modes[0].vdisplay;
		if (!lim_clk || c->modes[0].clock < lim_clk)
			lim_clk = c->modes[0].clock;
	}
	int n = drm_mode_table_count();
	for (int i = 0; i < n && c->nmodes < DRM_MAX_MODES; i++) {
		struct drm_mode_modeinfo m;
		int dup = 0;
		drm_mode_table_get(i, &m);
		if (m.flags & DRM_MODE_FLAG_INTERLACE)
			continue;
		if (!drm_mode_in_range(&m, c->edid_blob_id ? &r : NULL, lim_clk,
				       lim_w, lim_h))
			continue;
		for (uint32_t j = 0; j < c->nmodes; j++) {
			if (c->modes[j].hdisplay == m.hdisplay &&
			    c->modes[j].vdisplay == m.vdisplay &&
			    c->modes[j].vrefresh == m.vrefresh) {
				dup = 1;
				break;
			}
		}
		if (dup)
			continue;
		if (DRM_CONNECTOR_PROBE_HELPER) {
			/* the same checks as the EDID's modes get */
			struct drm_display_mode dm;

			if (drm_mode_from_umode(&dm, &m) != 0 ||
			    drm_connector_mode_valid(dev, c, &dm) != MODE_OK)
				continue;
		}
		if (drm_connector_add_mode(dev, conn, &m) == 0)
			added++;
	}
	return added;
}

/* ---- optional properties ---------------------------------------------------------
 *
 * Each is attached by a driver to the connectors that have what it
 * controls.  A client writes the mutable ones through the atomic state
 * (struct drm_connector_state); the connector keeps the committed value. */

int drm_connector_create_standard_properties(struct drm_device *dev)
{
	struct drm_prop *prop;

	if (!DRM_CONNECTOR_STD_PROPS_EARLY)
		return 0;

	if (!dev_blob_prop(dev, "PATH", true))
		return -ENOMEM;
	if (!dev_blob_prop(dev, "TILE", true))
		return -ENOMEM;
	if (!dev->prop_hdr_output_metadata) {
		prop = drm_property_create(dev, DRM_MODE_PROP_BLOB,
					   "HDR_OUTPUT_METADATA", 0);
		if (!prop)
			return -ENOMEM;
		dev->prop_hdr_output_metadata = prop->id;
	}
	return 0;
}

int drm_mode_create_content_type_property(struct drm_device *dev)
{
	struct drm_prop *prop;

	if (dev->prop_content_type)
		return 0;

	prop = drm_property_create_enum(dev, 0, "content type",
					drm_content_type_enum_list,
					ARRAY_SIZE(drm_content_type_enum_list));
	if (!prop)
		return -ENOMEM;
	dev->prop_content_type = prop->id;

	return 0;
}

int drm_connector_attach_content_type_property(struct drm_connector *c)
{
	struct drm_device *dev = c->dev;

	if (!drm_mode_create_content_type_property(dev)) {
		drm_object_attach_property(&c->properties,
					   drm_prop_find(dev, dev->prop_content_type),
					   DRM_MODE_CONTENT_TYPE_NO_DATA);
		c->content_type = DRM_MODE_CONTENT_TYPE_NO_DATA;
	}
	return 0;
}

int drm_connector_attach_scaling_mode_property(struct drm_connector *c,
					       uint32_t scaling_mode_mask)
{
	struct drm_device *dev = c->dev;
	struct drm_prop *scaling_mode_property;
	const unsigned int valid_scaling_mode_mask =
		(1U << ARRAY_SIZE(drm_scaling_mode_enum_list)) - 1;

	if (WARN_ON(gpu_hweight32(scaling_mode_mask) < 2 ||
		    scaling_mode_mask & ~valid_scaling_mode_mask))
		return -EINVAL;

	scaling_mode_property = drm_property_create(dev, DRM_MODE_PROP_ENUM,
						    "scaling mode",
						    (int)gpu_hweight32(scaling_mode_mask));
	if (!scaling_mode_property)
		return -ENOMEM;

	for (unsigned int i = 0; i < ARRAY_SIZE(drm_scaling_mode_enum_list); i++) {
		int ret;

		if (!(BIT(i) & scaling_mode_mask))
			continue;

		ret = drm_property_add_enum(scaling_mode_property,
					    (uint64_t)drm_scaling_mode_enum_list[i].type,
					    drm_scaling_mode_enum_list[i].name);
		if (ret) {
			drm_property_destroy(dev, scaling_mode_property);
			return ret;
		}
	}

	drm_object_attach_property(&c->properties, scaling_mode_property, 0);
	c->scaling_mode_property = scaling_mode_property;
	c->scaling_mode = DRM_MODE_SCALE_NONE;

	return 0;
}

int drm_connector_attach_vrr_capable_property(struct drm_connector *c)
{
	struct drm_prop *prop;

	if (!c->vrr_capable_property) {
		prop = drm_property_create_bool(c->dev, DRM_MODE_PROP_IMMUTABLE,
						"vrr_capable");
		if (!prop)
			return -ENOMEM;

		c->vrr_capable_property = prop;
		drm_object_attach_property(&c->properties, prop, 0);
	}

	return 0;
}

void drm_connector_set_vrr_capable_property(struct drm_connector *c, bool capable)
{
	if (!c->vrr_capable_property)
		return;

	drm_object_property_set_value(&c->properties, c->vrr_capable_property,
				      capable);
}

static int drm_mode_create_colorspace_property(struct drm_connector *c,
					       uint32_t supported_colorspaces)
{
	struct drm_device *dev = c->dev;
	uint32_t colorspaces = supported_colorspaces | BIT(DRM_MODE_COLORIMETRY_DEFAULT);
	struct drm_prop_enum_list enum_list[DRM_MODE_COLORIMETRY_COUNT];
	int i, len;

	if (c->colorspace_property)
		return 0;

	if (!supported_colorspaces) {
		kprintf("[drm] connector %u: no supported colorspaces given\n", c->id);
		return -EINVAL;
	}

	if ((supported_colorspaces & ~(BIT(DRM_MODE_COLORIMETRY_COUNT) - 1)) != 0) {
		kprintf("[drm] connector %u: unknown colorspace given\n", c->id);
		return -EINVAL;
	}

	len = 0;
	for (i = 0; i < DRM_MODE_COLORIMETRY_COUNT; i++) {
		if ((colorspaces & BIT(i)) == 0)
			continue;

		enum_list[len].type = i;
		enum_list[len].name = colorspace_names[i];
		len++;
	}

	c->colorspace_property = drm_property_create_enum(dev, DRM_MODE_PROP_ENUM,
							  "Colorspace",
							  enum_list, len);
	if (!c->colorspace_property)
		return -ENOMEM;

	return 0;
}

int drm_mode_create_hdmi_colorspace_property(struct drm_connector *c,
					     uint32_t supported_colorspaces)
{
	uint32_t colorspaces;

	if (supported_colorspaces)
		colorspaces = supported_colorspaces & hdmi_colorspaces;
	else
		colorspaces = hdmi_colorspaces;

	return drm_mode_create_colorspace_property(c, colorspaces);
}

int drm_mode_create_dp_colorspace_property(struct drm_connector *c,
					   uint32_t supported_colorspaces)
{
	uint32_t colorspaces;

	if (supported_colorspaces)
		colorspaces = supported_colorspaces & dp_colorspaces;
	else
		colorspaces = dp_colorspaces;

	return drm_mode_create_colorspace_property(c, colorspaces);
}

int drm_connector_attach_colorspace_property(struct drm_connector *c)
{
	if (WARN_ON(!c->colorspace_property))
		return -EINVAL;

	drm_object_attach_property(&c->properties, c->colorspace_property,
				   DRM_MODE_COLORIMETRY_DEFAULT);
	c->colorspace = DRM_MODE_COLORIMETRY_DEFAULT;

	return 0;
}

int drm_connector_attach_max_bpc_property(struct drm_connector *c, int min, int max)
{
	struct drm_prop *prop;

	prop = c->max_bpc_property;
	if (!prop) {
		prop = drm_property_create_range(c->dev, 0, "max bpc",
						 (uint64_t)min, (uint64_t)max);
		if (!prop)
			return -ENOMEM;

		c->max_bpc_property = prop;
	}

	drm_object_attach_property(&c->properties, prop, (uint64_t)max);
	c->max_requested_bpc = (uint32_t)max;
	c->max_bpc = (uint32_t)max;

	return 0;
}

int drm_connector_attach_hdr_output_metadata_property(struct drm_connector *c)
{
	struct drm_device *dev = c->dev;
	struct drm_prop *prop = NULL;

	if (dev->prop_hdr_output_metadata)
		prop = drm_prop_find(dev, dev->prop_hdr_output_metadata);
	if (!prop) {
		prop = drm_property_create(dev, DRM_MODE_PROP_BLOB,
					   "HDR_OUTPUT_METADATA", 0);
		if (!prop)
			return -ENOMEM;
		dev->prop_hdr_output_metadata = prop->id;
	}

	drm_object_attach_property(&c->properties, prop, 0);
	return 0;
}

bool drm_connector_atomic_hdr_metadata_equal(const struct drm_connector_state *old_state,
					     const struct drm_connector_state *new_state)
{
	const struct drm_blob *old_blob = old_state->hdr_output_metadata;
	const struct drm_blob *new_blob = new_state->hdr_output_metadata;

	if (!old_blob || !new_blob)
		return old_blob == new_blob;

	if (old_blob->length != new_blob->length)
		return false;

	return bytes_eq(old_blob->data, new_blob->data, old_blob->length);
}

int drm_connector_attach_broadcast_rgb_property(struct drm_connector *c)
{
	struct drm_prop *prop;

	prop = c->broadcast_rgb_property;
	if (!prop) {
		prop = drm_property_create_enum(c->dev, DRM_MODE_PROP_ENUM,
						"Broadcast RGB",
						broadcast_rgb_names,
						ARRAY_SIZE(broadcast_rgb_names));
		if (!prop)
			return -EINVAL;

		c->broadcast_rgb_property = prop;
	}

	drm_object_attach_property(&c->properties, prop,
				   DRM_HDMI_BROADCAST_RGB_AUTO);
	c->broadcast_rgb = DRM_HDMI_BROADCAST_RGB_AUTO;

	return 0;
}

int drm_connector_set_panel_orientation(struct drm_connector *c,
					enum drm_panel_orientation panel_orientation)
{
	struct drm_device *dev = c->dev;
	struct drm_display_info *info = &c->display_info;
	const uint32_t flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
	struct drm_prop *prop;

	/* Already set? */
	if (info->panel_orientation != DRM_MODE_PANEL_ORIENTATION_UNKNOWN)
		return 0;

	/* Don't attach the property if the orientation is unknown */
	if (panel_orientation == DRM_MODE_PANEL_ORIENTATION_UNKNOWN)
		return 0;

	info->panel_orientation = panel_orientation;

	prop = dev_prop_named(dev, "panel orientation", flags);
	if (!prop) {
		prop = drm_property_create_enum(dev, DRM_MODE_PROP_IMMUTABLE,
						"panel orientation",
						drm_panel_orientation_enum_list,
						ARRAY_SIZE(drm_panel_orientation_enum_list));
		if (!prop)
			return -ENOMEM;
	}

	drm_object_attach_property(&c->properties, prop,
				   (uint64_t)info->panel_orientation);
	return 0;
}

int drm_connector_attach_path_property(struct drm_connector *c)
{
	struct drm_prop *prop = dev_blob_prop(c->dev, "PATH", true);

	if (!prop)
		return -ENOMEM;
	if (!conn_attached_prop(c, prop->id))
		drm_object_attach_property(&c->properties, prop, 0);
	return 0;
}

int drm_connector_attach_tile_property(struct drm_connector *c)
{
	struct drm_prop *prop = dev_blob_prop(c->dev, "TILE", true);

	if (!prop)
		return -ENOMEM;
	if (!conn_attached_prop(c, prop->id)) {
		drm_object_attach_property(&c->properties, prop, 0);
		/* the EDID may have said already */
		return drm_connector_set_tile_property(c);
	}
	return 0;
}

int drm_connector_set_path_property(struct drm_connector *c, const char *path)
{
	struct drm_device *dev = c->dev;

	return drm_property_replace_global_blob(dev, &c->path_blob_ptr,
						kstrlen(path) + 1, path,
						&c->properties,
						dev_blob_prop(dev, "PATH", false));
}

int drm_connector_set_tile_property(struct drm_connector *c)
{
	struct drm_device *dev = c->dev;
	struct drm_prop *prop = dev_blob_prop(dev, "TILE", false);
	char tile[256];

	if (!c->has_tile)
		return drm_property_replace_global_blob(dev, &c->tile_blob_ptr, 0,
							NULL, &c->properties, prop);

	ksnprintf(tile, sizeof(tile), "%d:%d:%d:%d:%d:%d:%d:%d",
		  (int)c->tile_group_id, c->tile_is_single_monitor ? 1 : 0,
		  c->num_h_tile, c->num_v_tile, c->tile_h_loc, c->tile_v_loc,
		  c->tile_h_size, c->tile_v_size);

	return drm_property_replace_global_blob(dev, &c->tile_blob_ptr,
						kstrlen(tile) + 1, tile,
						&c->properties, prop);
}

void drm_connector_set_link_status_property(struct drm_connector *c,
					    uint64_t link_status)
{
	struct drm_prop *prop;

	c->link_status = link_status;
	prop = conn_attached_prop(c, c->dev->prop_link_status);
	if (prop)
		conn_list_store(c, prop, link_status);
}

/* ---- ioctls ----------------------------------------------------------------- */

/* GETENCODER */
int drm_mode_getencoder(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	(void)fp;
	struct drm_mode_get_encoder *e = kb;
	struct drm_encoder *en = drm_enc_find(dev, e->encoder_id);
	if (!en)
		return -ENOENT;
	e->encoder_type = en->type;
	e->crtc_id = en->crtc_id;
	e->possible_crtcs = en->possible_crtcs;
	e->possible_clones = 0;
	return 0;
}

static bool client_cap(const struct drm_file *fp, unsigned int cap)
{
	return ((fp->client_caps >> cap) & 1) != 0;
}

/* The probe a client asks for by passing no room for modes: the sink is
 * looked at again. */
static void connector_fill_modes(struct drm_device *dev, struct drm_connector *c)
{
	if (DRM_CONNECTOR_PROBE_HELPER &&
	    (dev->drv->features & DRM_FEATURE_PROBE_HELPER)) {
		drm_helper_probe_single_connector_modes(dev, c, dev->max_width,
							dev->max_height);
		return;
	}
	if (dev->drv->detect) {
		int was = c->connected;
		int st = drm_helper_probe_detect(dev, c, true);

		c->connected = (st == DRM_MODE_CONNECTED);
		/* the driver's hotplug code may not have seen it yet */
		if (was != c->connected)
			drm_kms_helper_hotplug_event(dev);
		if (dev->drv->get_modes)
			dev->drv->get_modes(dev, c);
	}
}

/* Which of the connector's modes this client is shown.  A stereo mode only
 * to a client that understands stereo layouts.  A client that does not
 * understand picture aspect ratios sees each timing once, without the
 * ratio: a mode that differs from one already shown only in its ratio is
 * left out.  `dm' has room for DRM_MAX_MODES (NULL: no filtering by ratio
 * possible, every mode shown). */
static uint32_t connector_expose_modes(struct drm_connector *c,
				       const struct drm_mode_modeinfo *modes,
				       uint32_t n, const struct drm_file *fp,
				       struct drm_display_mode *dm,
				       bool *expose)
{
	bool stereo_allowed = client_cap(fp, DRM_CLIENT_CAP_STEREO_3D);
	bool aspect_ratio_allowed = client_cap(fp, DRM_CLIENT_CAP_ASPECT_RATIO);
	uint32_t count = 0;

	(void)c;
	for (uint32_t i = 0; i < n; i++) {
		expose[i] = false;
		if (!stereo_allowed && (modes[i].flags & DRM_MODE_FLAG_3D_MASK))
			continue;
		if (!aspect_ratio_allowed && dm) {
			bool shown = false;

			if (drm_mode_from_umode(&dm[i], &modes[i]) == 0) {
				for (uint32_t j = 0; j < i && !shown; j++) {
					if (expose[j] &&
					    drm_mode_match(&dm[j], &dm[i],
							   DRM_MODE_MATCH_TIMINGS |
							   DRM_MODE_MATCH_CLOCK |
							   DRM_MODE_MATCH_FLAGS |
							   DRM_MODE_MATCH_3D_FLAGS))
						shown = true;
				}
			} else {
				/* a ratio no client form knows: never matched,
				 * keep it from matching anything later */
				mm_memset(&dm[i], 0, sizeof(dm[i]));
			}
			if (shown)
				continue;
		}
		expose[i] = true;
		count++;
	}
	return count;
}

static int connector_copy_modes(struct drm_connector *c, struct drm_file *fp,
				struct drm_mode_get_connector *g)
{
	bool aspect_ratio_allowed = client_cap(fp, DRM_CLIENT_CAP_ASPECT_RATIO);
	struct drm_mode_modeinfo *modes;
	struct drm_display_mode *dm;
	bool expose[DRM_MAX_MODES];
	uint32_t n = c->nmodes, count, room, copied = 0;
	int ret = 0;

	if (n > DRM_MAX_MODES)
		n = DRM_MAX_MODES;
	/* A copy, so that the list and the count agree even if a probe
	 * refills the connector meanwhile. */
	modes = kcalloc(n ? n : 1, sizeof(*modes));
	if (!modes)
		return -ENOMEM;
	mm_memcpy(modes, c->modes, n * sizeof(*modes));
	dm = aspect_ratio_allowed ? NULL : kcalloc(n ? n : 1, sizeof(*dm));
	count = connector_expose_modes(c, modes, n, fp, dm, expose);

	room = g->modes_ptr ? g->count_modes : 0;
	if (room > count)
		room = count;
	if (room) {
		uint64_t dst = g->modes_ptr;

		if (!validate_user_ptr(dst, room * sizeof(struct drm_mode_modeinfo))) {
			ret = -EFAULT;
			goto out;
		}
		for (uint32_t i = 0; i < n && copied < room; i++) {
			struct drm_mode_modeinfo u;

			if (!expose[i])
				continue;
			u = modes[i];
			/* the ratio only for a client that knows of it */
			if (!aspect_ratio_allowed)
				u.flags &= ~DRM_MODE_FLAG_PIC_AR_MASK;
			if (copy_to_user((void *)(uintptr_t)(dst + (uint64_t)copied * sizeof(u)),
					 &u, sizeof(u)) != 0) {
				ret = -EFAULT;
				goto out;
			}
			copied++;
		}
	}
	g->count_modes = count;
out:
	kfree(dm);
	kfree(modes);
	return ret;
}

/* GETCONNECTOR */
int drm_mode_getconnector(struct drm_device *dev, void *kb, struct drm_file *fp)
{
	struct drm_mode_get_connector *g = kb;
	struct drm_connector *c = drm_conn_find(dev, g->connector_id);
	uint32_t count_props;
	int rc;

	if (!c)
		return -ENOENT;
	/* The probe call -- no room for modes yet -- is where a
	 * client expects the sink to be looked at again. */
	if (g->count_modes == 0)
		connector_fill_modes(dev, c);
	uint32_t enc = c->encoder_id;
	if ((rc = drm_copy_ids(g->encoders_ptr, g->count_encoders, &enc, 1)))
		return rc;
	if ((rc = connector_copy_modes(c, fp, g)))
		return rc;
	/* The properties after the probe, so that the EDID and the rest
	 * describe what was just found. */
	count_props = g->count_props;
	rc = drm_mode_object_get_properties(dev, DRM_MODE_OBJECT_CONNECTOR, c,
					    client_cap(fp, DRM_CLIENT_CAP_ATOMIC),
					    g->props_ptr, g->prop_values_ptr,
					    &count_props);
	if (rc)
		return rc;
	g->encoder_id = c->encoder_id;
	g->connector_type = c->type;
	g->connector_type_id = c->type_id;
	g->connection = c->connected ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
	g->mm_width = c->mm_width;
	g->mm_height = c->mm_height;
	/* the panel's layout when the driver said, in the client's numbering
	 * (one above the order's own) */
	g->subpixel = c->display_info.subpixel_order != SubPixelUnknown ?
			      (uint32_t)c->display_info.subpixel_order + 1 :
			      DRM_MODE_SUBPIXEL_UNKNOWN;
	g->count_props = count_props;
	g->count_encoders = 1;
	return 0;
}

/* SETPROPERTY (connector): the same as OBJ_SETPROPERTY on the connector --
 * the property must be one it carries, the value one the property takes. */
int drm_connector_property_set_ioctl(struct drm_device *dev, void *kb,
				     struct drm_file *fp)
{
	struct drm_mode_connector_set_property *s = kb;
	struct drm_mode_obj_set_property obj_set_prop = {
		.value = s->value,
		.prop_id = s->prop_id,
		.obj_id = s->connector_id,
		.obj_type = DRM_MODE_OBJECT_CONNECTOR,
	};

	return drm_mode_obj_set_property_ioctl(dev, &obj_set_prop, fp);
}

/* ---- hotplug ------------------------------------------------------------ */

void drm_connector_hotplug(struct drm_device *dev, int conn)
{
	if (conn < 0 || (uint32_t)conn >= dev->nconn)
		return;
	struct drm_connector *c = &dev->conn[conn];
	int was = c->connected;
	if (DRM_CONNECTOR_PROBE_HELPER &&
	    (dev->drv->features & DRM_FEATURE_PROBE_HELPER)) {
		drm_helper_probe_single_connector_modes(dev, c, dev->max_width,
							dev->max_height);
	} else if (dev->drv->detect) {
		int st = drm_helper_probe_detect(dev, c, true);
		c->connected = (st == DRM_MODE_CONNECTED);
		if (c->connected && dev->drv->get_modes)
			dev->drv->get_modes(dev, c);
		else if (!c->connected) {
			drm_connector_set_edid(dev, conn, NULL, 0);
			drm_connector_clear_modes(dev, conn);
		}
	}
	drm_kms_helper_hotplug_event(dev);
	if (was != c->connected)
		kprintf("drm: connector %d %s (hotplug %u)\n", conn,
			c->connected ? "connected" : "disconnected", dev->hotplug_epoch);
}
