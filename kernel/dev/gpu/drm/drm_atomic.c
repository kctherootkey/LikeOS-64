// LikeOS -- display-manager core: atomic mode setting.
//
// A request is a state: every plane, CRTC and connector with the value it
// would have once the request is applied.  The state starts as a copy of
// what is committed, the caller's properties are written into it, the
// core checks that the whole makes sense (a plane on a crtc it can reach,
// a framebuffer big enough for its source rectangle, an active crtc with
// a mode and a connector), the driver checks that the hardware can show
// it, and then the driver applies it in one go.  Only after that do the
// objects take the new values, and the events go out at the next vblank.
//
// A property write must name a property the object carries, with a value
// the property allows (drm_property_change_valid_get); blobs a state names
// -- the mode, lookup tables, damage rectangles, HDR metadata -- are
// referenced by the state until it is freed, and by the object once it is
// committed.  Explicit fencing: IN_FENCE_FD hands a plane a sync_file the
// update waits for before it is applied; OUT_FENCE_PTR asks for a sync_file
// that signals once the crtc shows the update (its next vblank).
//
// The older calls -- SETCRTC, PAGE_FLIP, SETPLANE, CURSOR, DPMS, SETGAMMA
// and the kernel console's mode set -- are translated into such states
// here for a driver that has atomic entry points, so that driver has one
// path to program the hardware through, not six.
//
// A crtc state has two switches: enabled (mode_valid: a mode is set, the
// crtc has connectors and may have planes) and active (the pipe runs).
// DPMS off clears only active, so the planes keep their framebuffers and
// the next DPMS on shows the same picture; a crtc that is enabled but not
// active takes plane updates (a cursor moved while the screen is blanked)
// without showing them.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Red Hat's and Intel's code: MIT
// Portions Copyright (C) 2014 Red Hat
// Portions Copyright (C) 2014 Intel Corp.
// Portions Copyright (C) 2018 Intel Corp.
// Portions Copyright (c) 2016 Intel Corporation

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/gpu/drm_property.h>
#include <kernel/dev/gpu/drm_mode_object.h>
#include <kernel/dev/gpu/drm_atomic_helper.h>
#include <kernel/dev/gpu/drm_damage_helper.h>
#include <kernel/dev/gpu/drm_sync_file.h>
#include <kernel/dev/gpu/drm_color_mgmt.h>
#include <kernel/dev/gpu/drm_blend.h>
#include <kernel/dev/gpu/drm_vblank.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/syscalls.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* How long a commit waits for a plane's IN_FENCE_FD before it gives up
 * with -ETIMEDOUT.  The wait is interruptible, so a client can always get
 * out of it with a signal; the bound is for a fence that will never
 * signal (a device that died with work queued). */
#define ATOMIC_IN_FENCE_TIMEOUT_NS (10ULL * 1000000000ULL)

/* An OUT_FENCE_PTR fence signals at the crtc's next vblank after the
 * commit; should the counter stop (a crtc that stalls), it signals after
 * this long anyway rather than never. */
#define ATOMIC_OUT_FENCE_GIVE_UP_NS (1000ULL * 1000000ULL)
/* How often it looks again once the vblank it waits for is overdue. */
#define ATOMIC_OUT_FENCE_RECHECK_NS (500ULL * 1000ULL)
/* The error an out-fence of a failed request is signalled with: the
 * user ABI's ECANCELED (the kernel's errno list has no name for it). */
#define ATOMIC_OUT_FENCE_CANCELED (-125)

/* ---- the state --------------------------------------------------------- */

struct drm_plane *drm_crtc_primary(struct drm_device *dev, int crtc)
{
	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return NULL;
	return drm_plane_find(dev, dev->crtc[crtc].primary_plane_id);
}

struct drm_plane *drm_crtc_cursor(struct drm_device *dev, int crtc)
{
	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return NULL;
	return drm_plane_find(dev, dev->crtc[crtc].cursor_plane_id);
}

static struct drm_blob *blob_ref(struct drm_blob *b)
{
	return b ? drm_property_blob_get(b) : NULL;
}

/* The crtc index a connector's committed crtc id names, -1 none. */
static int conn_committed_crtc(struct drm_device *dev, const struct drm_connector *c)
{
	if (c->crtc_id) {
		struct drm_crtc *cr = drm_crtc_find(dev, c->crtc_id);
		if (cr)
			return cr->index;
	}
	return -1;
}

/*
 * The state starts as a copy of what is committed: each object's
 * committed values, with a reference on every blob they name, and none of
 * the per-request things (events, fences, damage, the "changed" flags).
 */
struct drm_atomic_state *drm_atomic_state_alloc(struct drm_device *dev,
						struct drm_file *fp,
						uint32_t flags)
{
	struct drm_atomic_state *st = kalloc(sizeof(*st));

	if (!st)
		return NULL;
	mm_memset(st, 0, sizeof(*st));
	st->dev = dev;
	st->fp = fp;
	st->flags = flags;
	for (uint32_t i = 0; i < dev->nplanes; i++) {
		struct drm_plane *p = &dev->planes[i];
		struct drm_plane_state *ps = &st->planes[i];
		ps->state = st;
		ps->plane = p;
		ps->crtc = p->crtc;
		ps->fb_id = p->fb_id;
		ps->fb = drm_fb_lookup(dev, p->fb_id);
		ps->src_x = p->src_x;
		ps->src_y = p->src_y;
		ps->src_w = p->src_w;
		ps->src_h = p->src_h;
		ps->crtc_x = p->crtc_x;
		ps->crtc_y = p->crtc_y;
		ps->crtc_w = p->crtc_w;
		ps->crtc_h = p->crtc_h;
		ps->hot_x = p->hot_x;
		ps->hot_y = p->hot_y;
		ps->rotation = p->rotation ? p->rotation : DRM_MODE_ROTATE_0;
		ps->zpos = p->zpos;
		ps->normalized_zpos = p->normalized_zpos;
		ps->alpha = p->alpha;
		ps->pixel_blend_mode = p->pixel_blend_mode;
		ps->color_encoding = p->color_encoding;
		ps->color_range = p->color_range;
	}
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc *c = &dev->crtc[i];
		struct drm_crtc_state *cs = &st->crtcs[i];
		cs->state = st;
		cs->crtc = c;
		cs->active = c->active;
		/* enabled: the crtc has a mode, whether or not its pipe runs */
		cs->mode_valid = DRM_ATOMIC_ENABLE_SEMANTICS ? (c->enabled || c->active) :
							      c->active;
		cs->mode = c->mode;
		cs->adjusted_mode = c->mode;
		mm_memcpy(cs->gamma, c->gamma, sizeof(cs->gamma));
		cs->gamma_identity = c->gamma_identity;
		cs->degamma_lut = blob_ref(c->degamma_lut);
		cs->ctm = blob_ref(c->ctm);
		cs->gamma_lut = blob_ref(c->gamma_lut);
		cs->vrr_enabled = c->vrr_enabled;
		cs->out_fence_fd = -1;
	}
	for (uint32_t i = 0; i < dev->ncrtc; i++)
		st->crtcs[i].plane_mask = drm_atomic_helper_crtc_plane_mask(st, (int)i);
	for (uint32_t i = 0; i < dev->nconn; i++) {
		struct drm_connector *c = &dev->conn[i];
		struct drm_connector_state *ns = &st->conns[i];
		ns->state = st;
		ns->conn = c;
		ns->crtc = conn_committed_crtc(dev, c);
		ns->dpms = c->dpms;
		ns->max_requested_bpc = c->max_requested_bpc;
		ns->colorspace = c->colorspace;
		ns->broadcast_rgb = c->broadcast_rgb;
		ns->content_type = c->content_type;
		ns->scaling_mode = c->scaling_mode;
		ns->hdr_output_metadata = blob_ref(c->hdr_output_metadata);
		/* the driver may mark the link bad at any time: read once */
		ns->link_status = __atomic_load_n(&c->link_status, __ATOMIC_RELAXED);
		ns->old_link_status = ns->link_status;
	}
	return st;
}

void drm_atomic_state_free(struct drm_atomic_state *st)
{
	if (!st)
		return;
	for (uint32_t i = 0; i < DRM_MAX_PLANES; i++) {
		struct drm_plane_state *ps = &st->planes[i];
		if (ps->in_fence)
			drm_fence_put(ps->in_fence);
		drm_property_blob_put(ps->fb_damage_clips_blob);
	}
	for (uint32_t i = 0; i < DRM_MAX_CRTCS; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		drm_property_blob_put(cs->mode_blob);
		drm_property_blob_put(cs->degamma_lut);
		drm_property_blob_put(cs->ctm);
		drm_property_blob_put(cs->gamma_lut);
		/* the ioctl hands its out-fences on or cancels them; one left
		 * here is only dropped */
		if (cs->out_fence)
			drm_fence_put(cs->out_fence);
	}
	for (uint32_t i = 0; i < DRM_MAX_CONNECTORS; i++)
		drm_property_blob_put(st->conns[i].hdr_output_metadata);
	kfree(st);
}

static int crtc_index_of(struct drm_device *dev, uint64_t id)
{
	if (!id)
		return -1;
	struct drm_crtc *c = drm_crtc_find(dev, (uint32_t)id);
	return c ? c->index : -2; /* -2: no such crtc */
}

/* The value of an immutable property attached to the object's list (a
 * LUT size): 0, or -EINVAL when the property does not exist or is not
 * attached. */
static int immutable_value(struct drm_device *dev,
			   const struct drm_object_properties *props,
			   uint32_t prop_id, uint64_t *val)
{
	struct drm_prop *p = prop_id ? drm_prop_find(dev, prop_id) : NULL;

	if (!p)
		return -EINVAL;
	return drm_object_property_get_value(props, p, val);
}

/* ---- property writes ----------------------------------------------------- */

/* Point the plane state's damage rectangles into its FB_DAMAGE_CLIPS blob
 * (none when there is no blob). */
static void plane_resolve_damage(struct drm_plane_state *ps)
{
	struct drm_blob *b = ps->fb_damage_clips_blob;

	ps->fb_damage_clips = b ? b->id : 0;
	ps->damage_clips = b ? (struct drm_mode_rect *)b->data : NULL;
	ps->num_damage_clips = b ? b->length / (uint32_t)sizeof(struct drm_mode_rect) : 0;
}

int drm_atomic_plane_set_property(struct drm_atomic_state *st,
				  struct drm_plane *p,
				  struct drm_prop *prop, uint64_t val)
{
	struct drm_device *dev = st->dev;
	struct drm_plane_state *ps = &st->planes[p->index];
	uint32_t id = prop->id;

	ps->changed = 1;
	if (id == dev->prop_fb_id) {
		struct drm_framebuffer *fb = NULL;
		if (val) {
			fb = drm_fb_lookup(dev, (uint32_t)val);
			if (!fb)
				return -ENOENT;
		}
		if (ps->fb_id != (uint32_t)val)
			ps->fb_changed = 1;
		ps->fb_id = (uint32_t)val;
		ps->fb = fb;
		return 0;
	}
	if (id == dev->prop_in_fence_fd) {
		/* one fence per plane and request; -1 is "none" */
		if (ps->in_fence)
			return -EINVAL;
		if ((int64_t)val == -1)
			return 0;
		if ((int64_t)val < 0 || val > 0x7fffffffULL)
			return -EINVAL;
		ps->in_fence = drm_fence_from_fd((int)val);
		if (!ps->in_fence)
			return -EINVAL;
		return 0;
	}
	if (id == dev->prop_crtc_id) {
		int idx = crtc_index_of(dev, val);
		if (idx == -2)
			return -ENOENT;
		if (ps->crtc != idx)
			ps->crtc_changed = 1;
		ps->crtc = idx;
		return 0;
	}
	if (id == dev->prop_fb_damage_clips) {
		bool replaced = false;
		int rc = drm_property_replace_blob_from_id(dev, &ps->fb_damage_clips_blob,
							   val, -1, -1,
							   sizeof(struct drm_mode_rect),
							   &replaced);
		if (rc)
			return rc;
		plane_resolve_damage(ps);
		return 0;
	}
	ps->geometry_changed = 1;
	if (id == dev->prop_src_x)
		ps->src_x = (uint32_t)val;
	else if (id == dev->prop_src_y)
		ps->src_y = (uint32_t)val;
	else if (id == dev->prop_src_w)
		ps->src_w = (uint32_t)val;
	else if (id == dev->prop_src_h)
		ps->src_h = (uint32_t)val;
	else if (id == dev->prop_crtc_x)
		ps->crtc_x = (int32_t)val;
	else if (id == dev->prop_crtc_y)
		ps->crtc_y = (int32_t)val;
	else if (id == dev->prop_crtc_w)
		ps->crtc_w = (uint32_t)val;
	else if (id == dev->prop_crtc_h)
		ps->crtc_h = (uint32_t)val;
	else if (prop == p->alpha_property)
		ps->alpha = (uint16_t)val;
	else if (prop == p->blend_mode_property)
		ps->pixel_blend_mode = (uint16_t)val;
	else if (prop == p->rotation_property) {
		/* exactly one rotation, any reflections */
		if (!is_power_of_2(val & DRM_MODE_ROTATE_MASK))
			return -EINVAL;
		ps->rotation = (uint32_t)val;
	} else if (prop == p->zpos_property)
		ps->zpos = (uint32_t)val;
	else if (prop == p->color_encoding_property)
		ps->color_encoding = (uint32_t)val;
	else if (prop == p->color_range_property)
		ps->color_range = (uint32_t)val;
	else if (DRM_CURSOR_HOTSPOT && p->hotspot_x_property && prop == p->hotspot_x_property) {
		if (p->type != DRM_PLANE_TYPE_CURSOR)
			return -EINVAL;
		ps->hot_x = (int32_t)val;
	} else if (DRM_CURSOR_HOTSPOT && p->hotspot_y_property && prop == p->hotspot_y_property) {
		if (p->type != DRM_PLANE_TYPE_CURSOR)
			return -EINVAL;
		ps->hot_y = (int32_t)val;
	} else
		return -EINVAL; /* immutable or unknown */
	return 0;
}

int drm_atomic_plane_set(struct drm_atomic_state *st, struct drm_plane *p,
			 uint32_t prop, uint64_t val)
{
	struct drm_prop *pr = drm_prop_find(st->dev, prop);

	if (!pr) {
		st->planes[p->index].changed = 1;
		return -EINVAL;
	}
	return drm_atomic_plane_set_property(st, p, pr, val);
}

/* A table of `n' entries, resampled onto the crtc's 256. */
static void gamma_from_lut(struct drm_crtc_state *cs, const struct drm_color_lut *lut,
			   uint32_t n)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t k = n == 256 ? i : (uint32_t)(((uint64_t)i * (n - 1) + 127) / 255);
		if (k >= n)
			k = n - 1;
		cs->gamma[0][i] = lut[k].red;
		cs->gamma[1][i] = lut[k].green;
		cs->gamma[2][i] = lut[k].blue;
	}
}

static void gamma_identity(struct drm_crtc_state *cs)
{
	for (uint32_t i = 0; i < 256; i++) {
		cs->gamma[0][i] = (uint16_t)(i << 8 | i);
		cs->gamma[1][i] = (uint16_t)(i << 8 | i);
		cs->gamma[2][i] = (uint16_t)(i << 8 | i);
	}
}

static int gamma_is_identity(const struct drm_crtc_state *cs)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint16_t v = (uint16_t)(i << 8 | i);
		/* the high byte is what an 8-bit table keeps */
		if ((cs->gamma[0][i] >> 8) != (v >> 8) ||
		    (cs->gamma[1][i] >> 8) != (v >> 8) ||
		    (cs->gamma[2][i] >> 8) != (v >> 8))
			return 0;
	}
	return 1;
}

/* MODE_ID: the mode of a blob (referenced by the state from now on), or
 * no mode for 0.  The blob must hold exactly one client-form mode whose
 * timings make sense. */
static int crtc_set_mode_prop(struct drm_atomic_state *st, struct drm_crtc_state *cs,
			      uint64_t val)
{
	struct drm_device *dev = st->dev;
	struct drm_display_mode dm;
	struct drm_mode_modeinfo m;
	struct drm_blob *b;

	if (!val) {
		if (cs->mode_valid)
			cs->mode_changed = 1;
		cs->mode_valid = 0;
		drm_property_replace_blob(&cs->mode_blob, NULL);
		return 0;
	}
	b = val <= 0xffffffffULL ? drm_property_lookup_blob(dev, (uint32_t)val) : NULL;
	if (!b)
		return -ENOENT;
	if (b->length != sizeof(struct drm_mode_modeinfo)) {
		drm_property_blob_put(b);
		return -EINVAL;
	}
	mm_memcpy(&m, b->data, sizeof(m));
	if (drm_mode_convert_umode(dev, &dm, &m)) {
		drm_property_blob_put(b);
		return -EINVAL;
	}
	if (!cs->mode_valid || !drm_mode_equal(&cs->mode, &m))
		cs->mode_changed = 1;
	cs->mode = m;
	cs->adjusted_mode = m;
	cs->mode_valid = 1;
	drm_property_replace_blob(&cs->mode_blob, b);
	drm_property_blob_put(b);
	return 0;
}

/* GAMMA_LUT: at most GAMMA_LUT_SIZE entries of struct drm_color_lut, kept
 * as the blob and, for the drivers, resampled into the crtc's 256-entry
 * table; 0 is the identity. */
static int crtc_set_gamma_lut(struct drm_atomic_state *st, struct drm_crtc *c,
			      struct drm_crtc_state *cs, uint64_t val)
{
	struct drm_device *dev = st->dev;
	const uint64_t elem = sizeof(struct drm_color_lut);
	struct drm_object_properties *props;
	bool replaced = false;
	uint64_t lut_size;
	int rc;

	props = drm_mode_object_props_of(dev, DRM_MODE_OBJECT_CRTC, c);
	rc = immutable_value(dev, props, dev->prop_gamma_lut_size, &lut_size);
	if (rc)
		return rc;
	rc = drm_property_replace_blob_from_id(dev, &cs->gamma_lut, val,
					       (int64_t)(elem * lut_size), -1,
					       (int64_t)elem, &replaced);
	if (rc)
		return rc;
	cs->color_mgmt_changed |= replaced;
	if (!cs->gamma_lut) {
		gamma_identity(cs);
		cs->gamma_identity = 1;
		cs->gamma_changed = 1;
		return 0;
	}
	uint32_t n = cs->gamma_lut->length / (uint32_t)elem;
	if (n < 2)
		return -EINVAL;
	gamma_from_lut(cs, cs->gamma_lut->data, n);
	cs->gamma_identity = gamma_is_identity(cs);
	cs->gamma_changed = 1;
	return 0;
}

int drm_atomic_crtc_set_property(struct drm_atomic_state *st,
				 struct drm_crtc *c,
				 struct drm_prop *prop, uint64_t val)
{
	struct drm_device *dev = st->dev;
	struct drm_crtc_state *cs = &st->crtcs[c->index];
	uint32_t id = prop->id;
	bool replaced = false;
	int rc;

	cs->changed = 1;
	if (id == dev->prop_active) {
		int active = val ? 1 : 0;
		if (cs->active != active)
			cs->active_changed = 1;
		cs->active = active;
		return 0;
	}
	if (id == dev->prop_mode_id)
		return crtc_set_mode_prop(st, cs, val);
	if (id == dev->prop_vrr_enabled) {
		cs->vrr_enabled = val ? 1 : 0;
		return 0;
	}
	if (id == dev->prop_degamma_lut) {
		const uint64_t elem = sizeof(struct drm_color_lut);
		uint64_t lut_size;

		rc = immutable_value(dev, drm_mode_object_props_of(dev, DRM_MODE_OBJECT_CRTC, c),
				     dev->prop_degamma_lut_size, &lut_size);
		if (rc)
			return rc;
		rc = drm_property_replace_blob_from_id(dev, &cs->degamma_lut, val,
						       (int64_t)(elem * lut_size), -1,
						       (int64_t)elem, &replaced);
		cs->color_mgmt_changed |= replaced;
		return rc;
	}
	if (id == dev->prop_ctm) {
		rc = drm_property_replace_blob_from_id(dev, &cs->ctm, val, -1,
						       sizeof(struct drm_color_ctm), -1,
						       &replaced);
		cs->color_mgmt_changed |= replaced;
		return rc;
	}
	if (id == dev->prop_gamma_lut)
		return crtc_set_gamma_lut(st, c, cs, val);
	if (id == dev->prop_out_fence_ptr) {
		int32_t none = -1;

		if (!val)
			return 0;
		/* -1 until the request succeeds: the pointer must be
		 * writable now, not only once the commit is done */
		if (!validate_user_ptr(val, sizeof(int32_t)) ||
		    copy_to_user((void *)(uintptr_t)val, &none, sizeof(none)) != 0)
			return -EFAULT;
		cs->out_fence_ptr = val;
		return 0;
	}
	return -EINVAL; /* immutable or unknown */
}

int drm_atomic_crtc_set(struct drm_atomic_state *st, struct drm_crtc *c,
			uint32_t prop, uint64_t val)
{
	struct drm_prop *pr = drm_prop_find(st->dev, prop);

	if (!pr) {
		st->crtcs[c->index].changed = 1;
		return -EINVAL;
	}
	return drm_atomic_crtc_set_property(st, c, pr, val);
}

int drm_atomic_conn_set_property(struct drm_atomic_state *st,
				 struct drm_connector *c,
				 struct drm_prop *prop, uint64_t val)
{
	struct drm_device *dev = st->dev;
	int idx = (int)(c - dev->conn);
	struct drm_connector_state *ns = &st->conns[idx];
	uint32_t id = prop->id;

	ns->changed = 1;
	if (id == dev->prop_crtc_id) {
		int ci = crtc_index_of(dev, val);
		if (ci == -2)
			return -ENOENT;
		ns->crtc = ci;
		return 0;
	}
	if (id == dev->prop_dpms) {
		/* The older knob on an atomic driver: on = the connector's
		 * crtc active, anything else = off.  The value itself is
		 * remembered for GETCONNECTOR.  (The ATOMIC ioctl itself
		 * refuses DPMS; this is for the legacy paths.) */
#if DRM_ATOMIC_ENABLE_SEMANTICS
		/* Standby and suspend are off: there is no state between.
		 * The crtc runs while any connector on it is on; its mode,
		 * planes and connectors stay as they are. */
		int mode = val == 0 ? 0 : 3;

		ns->dpms = mode;
		ns->dpms_changed = 1;
		if (ns->crtc >= 0) {
			struct drm_crtc_state *cs = &st->crtcs[ns->crtc];
			int on = 0;

			for (uint32_t i = 0; i < dev->nconn; i++)
				if (st->conns[i].crtc == ns->crtc && st->conns[i].dpms == 0)
					on = 1;
			cs->changed = 1;
			if (cs->active != on)
				cs->active_changed = 1;
			cs->active = on;
		}
#else
		int on = (val == 0);
		ns->dpms = (int)val;
		if (ns->crtc >= 0) {
			struct drm_crtc_state *cs = &st->crtcs[ns->crtc];
			cs->changed = 1;
			if (cs->active != on)
				cs->active_changed = 1;
			cs->active = on;
		}
#endif
		return 0;
	}
	if (id == dev->prop_link_status) {
#if DRM_LINK_STATUS_SEMANTICS
		/* Only the hardware makes a link bad; a client may only ask
		 * for a bad one to be trained again (GOOD).  A client that
		 * restores a state it saved writes back whatever it read --
		 * it need not understand the property -- so a write of BAD
		 * is accepted and changes nothing, rather than breaking the
		 * display on a VT switch. */
		if (ns->link_status != DRM_MODE_LINK_STATUS_GOOD)
			ns->link_status = val;
#else
		/* A client restoring a state writes back what it read; the
		 * link here is never reported bad, and a client may not make
		 * it so: accepted, nothing changes. */
#endif
		return 0;
	}
	if (id == dev->prop_hdr_output_metadata) {
		bool replaced = false;
		return drm_property_replace_blob_from_id(dev, &ns->hdr_output_metadata,
							 val, -1,
							 sizeof(struct hdr_output_metadata),
							 -1, &replaced);
	}
	if (id == dev->prop_content_type) {
		ns->content_type = (uint32_t)val;
		return 0;
	}
	if (prop == c->scaling_mode_property)
		ns->scaling_mode = (uint32_t)val;
	else if (prop == c->colorspace_property)
		ns->colorspace = (uint32_t)val;
	else if (prop == c->max_bpc_property)
		ns->max_requested_bpc = (uint32_t)val;
	else if (prop == c->broadcast_rgb_property)
		ns->broadcast_rgb = (uint32_t)val;
	else
		return -EINVAL; /* immutable (EDID, non-desktop) or unknown */
	return 0;
}

int drm_atomic_conn_set(struct drm_atomic_state *st, struct drm_connector *c,
			uint32_t prop, uint64_t val)
{
	struct drm_prop *pr = drm_prop_find(st->dev, prop);

	if (!pr) {
		st->conns[c - st->dev->conn].changed = 1;
		return -EINVAL;
	}
	return drm_atomic_conn_set_property(st, c, pr, val);
}

int drm_atomic_set_property(struct drm_atomic_state *st, uint32_t type,
			    void *obj, struct drm_prop *prop, uint64_t val)
{
	struct drm_device *dev = st->dev;
	void *ref;
	int rc;

	if (!drm_property_change_valid_get(prop, val, &ref))
		return -EINVAL;
	switch (type) {
	case DRM_MODE_OBJECT_CONNECTOR:
		/* DPMS has an older meaning of its own that only the legacy
		 * interface (SETPROPERTY) gives it */
		if (prop->id == dev->prop_dpms)
			rc = -EINVAL;
		else
			rc = drm_atomic_conn_set_property(st, obj, prop, val);
		break;
	case DRM_MODE_OBJECT_CRTC:
		rc = drm_atomic_crtc_set_property(st, obj, prop, val);
		break;
	case DRM_MODE_OBJECT_PLANE:
		rc = drm_atomic_plane_set_property(st, obj, prop, val);
		break;
	default:
		rc = -EINVAL;
		break;
	}
	drm_property_change_valid_put(prop, ref);
	return rc;
}

/* ---- the check ------------------------------------------------------------ */

static int plane_takes(const struct drm_plane *p, uint32_t format, uint64_t modifier)
{
	uint32_t i;
	for (i = 0; i < p->nformats; i++)
		if (p->formats[i] == format)
			break;
	if (i == p->nformats)
		return 0;
	for (i = 0; i < p->nmodifiers; i++)
		if (p->modifiers[i] == modifier)
			return 1;
	return 0;
}

/* A state refused, said in the log (the first few times): a client only
 * sees the errno, and a display server that dies of it names nothing. */
static int check_fail(const char *why, int rc)
{
	static unsigned said;
	if (said < 8) {
		said++;
		kprintf("[drm] atomic check refused: %s\n", why);
	}
	return rc;
}

/* The checks every plane with a framebuffer gets, whatever the driver:
 * coordinates that cannot overflow, a source inside the framebuffer, and
 * damage rectangles inside it too. */
static int plane_check_common(const struct drm_plane_state *ps)
{
	const struct drm_framebuffer *fb = ps->fb;
	uint32_t fb_width = fb->width << 16;
	uint32_t fb_height = fb->height << 16;
	const struct drm_mode_rect *clips;
	uint32_t num_clips;
	int rc;

	/* give drivers some help against integer overflows */
	if (ps->crtc_w > 0x7fffffffU ||
	    ps->crtc_x > (int32_t)(0x7fffffffU - ps->crtc_w) ||
	    ps->crtc_h > 0x7fffffffU ||
	    ps->crtc_y > (int32_t)(0x7fffffffU - ps->crtc_h))
		return check_fail("plane coordinates that overflow", -ERANGE);

	rc = drm_framebuffer_check_src_coords(ps->src_x, ps->src_y, ps->src_w,
					      ps->src_h, fb);
	if (rc)
		return check_fail("the source rectangle is outside the framebuffer", rc);

	clips = drm_plane_get_damage_clips(ps);
	num_clips = drm_plane_get_damage_clips_count(ps);
	for (; num_clips > 0; clips++, num_clips--) {
		if (clips->x1 >= clips->x2 || clips->y1 >= clips->y2 ||
		    clips->x1 < 0 || clips->y1 < 0 ||
		    (int64_t)clips->x2 > (int64_t)fb_width ||
		    (int64_t)clips->y2 > (int64_t)fb_height)
			return check_fail("a damage rectangle outside the framebuffer", -EINVAL);
	}
	return 0;
}

/* What the connector state settles from the sink and the request. */
static void connector_check(struct drm_connector_state *ns)
{
	const struct drm_connector *c = ns->conn;

	ns->max_bpc = c->display_info.bpc ? c->display_info.bpc : 8;
	if (c->max_bpc_property && ns->max_requested_bpc < ns->max_bpc)
		ns->max_bpc = ns->max_requested_bpc;
}

int drm_atomic_check(struct drm_atomic_state *st)
{
	struct drm_device *dev = st->dev;
	int plane_check = (dev->drv->features & DRM_FEATURE_ATOMIC_PLANE_CHECK) != 0;
	uint32_t i;
	int rc;

	/* Connectors: which crtc each is on, and so which crtcs have one. */
	for (i = 0; i < dev->ncrtc; i++)
		st->crtcs[i].connector_mask = 0;
	for (i = 0; i < dev->nconn; i++) {
		struct drm_connector_state *ns = &st->conns[i];
		int was = conn_committed_crtc(dev, ns->conn);
		if (ns->crtc >= (int)dev->ncrtc)
			return check_fail("a connector names no crtc", -EINVAL);
		if (ns->crtc >= 0)
			st->crtcs[ns->crtc].connector_mask |= 1u << i;
		if (was != ns->crtc) {
			/* Moving a connector is a mode set on both ends. */
			if (was >= 0) {
				st->crtcs[was].changed = 1;
				st->crtcs[was].connectors_changed = 1;
			}
			if (ns->crtc >= 0) {
				st->crtcs[ns->crtc].changed = 1;
				st->crtcs[ns->crtc].connectors_changed = 1;
			}
		}
#if DRM_LINK_STATUS_SEMANTICS
		/* A link set GOOD again is trained again: a full mode set of
		 * the crtc the connector was on.  (Without ALLOW_MODESET the
		 * request is refused below.) */
		if (ns->link_status != ns->old_link_status && was >= 0) {
			st->crtcs[was].changed = 1;
			st->crtcs[was].connectors_changed = 1;
		}
#endif
		connector_check(ns);
	}
	/* CRTCs. */
	for (i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		if (cs->active && !cs->mode_valid)
			return check_fail("an active crtc has no mode", -EINVAL);
#if DRM_ATOMIC_ENABLE_SEMANTICS
		/* A mode needs somewhere to go, and a connector driven by a
		 * crtc needs the crtc to have a mode. */
		if (cs->mode_valid != (cs->connector_mask != 0))
			return check_fail(cs->mode_valid ? "an enabled crtc has no connector" :
							  "a connector on a crtc without a mode",
					  -EINVAL);
#else
		if (cs->active && !cs->connector_mask)
			return check_fail("an active crtc has no connector", -EINVAL);
#endif
		if (cs->mode_valid) {
			const struct drm_mode_modeinfo *m = &cs->mode;
			if (!m->hdisplay || !m->vdisplay || !m->htotal || !m->vtotal ||
			    !m->clock || m->hdisplay > m->htotal ||
			    m->vdisplay > m->vtotal ||
			    m->hdisplay > dev->max_width || m->vdisplay > dev->max_height)
				return check_fail("the mode's timings are not possible", -EINVAL);
		}
		/* An event (or an out-fence) for a crtc that is off and
		 * stays off would never come: refused, rather than leaving
		 * the client waiting for it. */
		if ((cs->event || cs->out_fence_ptr) && !cs->active && !cs->crtc->active)
			return check_fail("an event for a crtc that stays off", -EINVAL);
		if (drm_crtc_state_needs_modeset(cs)) {
			if (!st->allow_modeset)
				return check_fail("a mode set without ALLOW_MODESET", -EINVAL);
			cs->changed = 1;
		}
		cs->plane_mask = drm_atomic_helper_crtc_plane_mask(st, (int)i);
	}
	/* Planes. */
	for (i = 0; i < dev->nplanes; i++) {
		struct drm_plane_state *ps = &st->planes[i];
		struct drm_plane *p = ps->plane;
		if (ps->crtc >= (int)dev->ncrtc)
			return check_fail("a plane names no crtc", -EINVAL);
		if (ps->crtc >= 0 && !(p->possible_crtcs & (1u << ps->crtc)))
			return check_fail("a plane on a crtc it cannot be on", -EINVAL);
		/* a framebuffer needs a crtc and a crtc a framebuffer */
		if ((ps->fb != NULL) != (ps->crtc >= 0))
			return check_fail("a plane with a framebuffer and no crtc, or the reverse", -EINVAL);
		if (ps->changed && ps->crtc >= 0)
			st->crtcs[ps->crtc].changed = 1;
		if (ps->crtc_changed && p->crtc >= 0 && p->crtc != ps->crtc)
			st->crtcs[p->crtc].changed = 1;
		if (!ps->fb) {
			if (plane_check) {
				/* switched off: not visible */
				rc = drm_atomic_helper_check_plane_state(ps, NULL,
									 DRM_PLANE_NO_SCALING,
									 DRM_PLANE_NO_SCALING,
									 true, true);
				if (rc)
					return check_fail("the plane check", rc);
			}
			continue;
		}
		struct drm_crtc_state *cs = &st->crtcs[ps->crtc];
#if DRM_ATOMIC_ENABLE_SEMANTICS
		/* a crtc that is only switched off (DPMS) keeps its planes */
		if (!cs->mode_valid)
			return check_fail("a plane on a crtc without a mode", -EINVAL);
#else
		if (!cs->active)
			return check_fail("a plane on a crtc that is off", -EINVAL);
#endif
		if (!plane_takes(p, ps->fb->format, ps->fb->modifier))
			return check_fail("the plane does not take the framebuffer's format or layout", -EINVAL);
		rc = plane_check_common(ps);
		if (rc)
			return rc;
		if (plane_check) {
			/* Clipped against the crtc: the source and destination
			 * the driver shows, and whether anything is visible.
			 * No scaling inside a plane; only the primary plane
			 * has to cover the whole crtc. */
			rc = drm_atomic_helper_check_plane_state(ps, cs,
								 DRM_PLANE_NO_SCALING,
								 DRM_PLANE_NO_SCALING,
								 p->type != DRM_PLANE_TYPE_PRIMARY,
								 true);
			if (rc)
				return check_fail("the plane does not fit its crtc", rc);
		} else {
			if (!ps->src_w || !ps->src_h || !ps->crtc_w || !ps->crtc_h)
				return check_fail("a plane rectangle of size zero", -EINVAL);
			/* no scaling inside a plane: the source and the
			 * destination are the same size (the pipe scaler is a
			 * crtc matter) */
			if ((ps->src_w >> 16) != ps->crtc_w || (ps->src_h >> 16) != ps->crtc_h)
				return check_fail("a plane would have to scale", -EINVAL);
		}
		if (p->type == DRM_PLANE_TYPE_CURSOR && dev->drv->cursor_w &&
		    (ps->crtc_w > dev->drv->cursor_w || ps->crtc_h > dev->drv->cursor_h))
			return check_fail("the cursor is larger than the device's", -EINVAL);
	}
	if (plane_check) {
		for (i = 0; i < dev->ncrtc; i++) {
			struct drm_crtc_state *cs = &st->crtcs[i];
			if (cs->mode_valid && (DRM_ATOMIC_ENABLE_SEMANTICS || cs->active) &&
			    drm_atomic_helper_check_crtc_primary_plane(st, cs))
				return check_fail("an enabled crtc without its primary plane", -EINVAL);
		}
	}
	/* Stacking: every crtc's planes numbered 0..n-1 by zpos, for a driver
	 * whose planes carry it; a crtc whose stacking changed is part of
	 * the update. */
	if (dev->drv->features & DRM_FEATURE_BLEND) {
		uint32_t zmask = 0;

		rc = drm_atomic_normalize_zpos_crtcs(dev, st, &zmask);
		if (rc)
			return check_fail("a plane stacking that cannot be normalised", rc);
		for (i = 0; i < dev->ncrtc; i++)
			if (zmask & (1u << i))
				st->crtcs[i].changed = 1;
	}
	/* The damage a client gave means nothing across a full mode set:
	 * everything is redrawn.  After the mode-set flags are known. */
	for (i = 0; i < dev->nplanes; i++)
		drm_atomic_helper_check_plane_damage(st, &st->planes[i]);
	if (dev->drv->atomic_check) {
		rc = dev->drv->atomic_check(dev, st);
		if (rc)
			return rc;
	}
	/* The driver may have turned an update into a mode set. */
	if (!st->allow_modeset) {
		for (i = 0; i < dev->ncrtc; i++)
			if (drm_crtc_state_needs_modeset(&st->crtcs[i]))
				return check_fail("a mode set without ALLOW_MODESET", -EINVAL);
	}
	return 0;
}

/* ---- explicit fences ------------------------------------------------------
 *
 * An OUT_FENCE_PTR fence stands for "the crtc shows this update": the
 * vblank after the commit, the moment the flip event goes out.  It is a
 * fence of no device list; a timer of its own watches the crtc's vblank
 * counter and signals it.  The timer holds a reference while it is armed
 * and is never cancelled: it runs until it signals the fence (the vblank
 * came, the crtc went off, or it gave up waiting) and then drops that
 * reference.  Where the driver switches its vblank interrupt off between
 * users, the armed fence is such a user (a vblank reference, dropped as it
 * signals): without one the counter would stand still and the fence would
 * only ever signal by giving up.
 */

struct atomic_out_fence {
	struct drm_fence base; /* first: the default release frees the whole */
	struct drm_device *dev;
	int crtc;
	volatile int armed; /* target and give_up are set */
	int holds_vblank_ref; /* a drm_crtc_vblank_get the timer puts */
	uint64_t target; /* the vblank count it signals at */
	uint64_t give_up_ns;
	hrtimer_t timer;
};

static bool out_fence_done(struct atomic_out_fence *of)
{
	struct drm_device *dev = of->dev;

	if (!__atomic_load_n(&of->armed, __ATOMIC_ACQUIRE))
		return false;
	return drm_vblank_passed(__atomic_load_n(&dev->vbl[of->crtc].count,
						 __ATOMIC_RELAXED), of->target) ||
	       !dev->crtc[of->crtc].active || hrtimer_now_ns() >= of->give_up_ns;
}

static bool out_fence_signaled(struct drm_fence *f)
{
	return out_fence_done((struct atomic_out_fence *)f);
}

static bool out_fence_enable_signaling(struct drm_fence *f)
{
	/* the timer is what watches; it is armed by the commit */
	return !out_fence_done((struct atomic_out_fence *)f);
}

static const char *out_fence_driver_name(struct drm_fence *f)
{
	struct atomic_out_fence *of = (struct atomic_out_fence *)f;

	return of->dev->drv && of->dev->drv->name ? of->dev->drv->name : "drm";
}

static const struct drm_fence_ops out_fence_ops = {
	.get_driver_name = out_fence_driver_name,
	.signaled = out_fence_signaled,
	.enable_signaling = out_fence_enable_signaling,
};

/* Interrupt context, interrupts off. */
static void out_fence_timer_fire(hrtimer_t *t)
{
	struct atomic_out_fence *of = t->arg;

	if (!out_fence_done(of)) {
		hrtimer_start(t, hrtimer_now_ns() + ATOMIC_OUT_FENCE_RECHECK_NS);
		return;
	}
	/* the counter need not count for this fence any more (put is safe
	 * here: it never sleeps) */
	if (of->holds_vblank_ref) {
		of->holds_vblank_ref = 0;
		drm_crtc_vblank_put(of->dev, of->crtc);
	}
	drm_fence_signal(&of->base);
	drm_fence_put(&of->base); /* the timer's */
}

/* A fresh out-fence on the crtc's stream (one reference, the caller's). */
static struct drm_fence *out_fence_create(struct drm_device *dev, int crtc)
{
	struct drm_crtc *c = &dev->crtc[crtc];
	struct atomic_out_fence *of;
	uint64_t ctx = __atomic_load_n(&c->fence_context, __ATOMIC_ACQUIRE);

	if (!ctx) {
		uint64_t fresh = drm_fence_context_alloc(1);
		uint64_t expected = 0;
		if (__atomic_compare_exchange_n(&c->fence_context, &expected, fresh,
						false, __ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			ctx = fresh;
		else
			ctx = expected;
	}
	of = kalloc(sizeof(*of));
	if (!of)
		return NULL;
	mm_memset(of, 0, sizeof(*of));
	drm_fence_init_unlisted(&of->base, dev, &out_fence_ops, ctx,
				__atomic_add_fetch(&c->fence_seqno, 1, __ATOMIC_ACQ_REL));
	ksnprintf(of->base.name, sizeof(of->base.name), "crtc-%d", crtc);
	of->dev = dev;
	of->crtc = crtc;
	hrtimer_init(&of->timer, out_fence_timer_fire, of);
	return &of->base;
}

/* The commit is done: the fence signals at the crtc's next vblank, the
 * one its flip event goes out at. */
static void out_fence_arm(struct drm_fence *f)
{
	struct atomic_out_fence *of = (struct atomic_out_fence *)f;
	struct drm_device *dev = of->dev;
	struct drm_vblank_crtc *v = &dev->vbl[of->crtc];
	uint64_t now = hrtimer_now_ns();
	uint64_t period = v->period_ns ? v->period_ns : 16666666ULL;
	uint64_t due;

	if (of->armed)
		return;
#if DRM_OUT_FENCE_VBLANK_REF
	/* The reference first: switching the interrupt on brings the count
	 * up to date, and the target is the vblank after the one the
	 * hardware is in now.  A crtc that refuses is off, and the fence
	 * signals at the first look. */
	if (dev->drv->enable_vblank && drm_crtc_vblank_get(dev, of->crtc) == 0) {
		of->holds_vblank_ref = 1;
		of->target = drm_crtc_accurate_vblank_count(dev, of->crtc) + 1;
	} else
#endif
		of->target = __atomic_load_n(&v->count, __ATOMIC_RELAXED) + 1;
	of->give_up_ns = now + ATOMIC_OUT_FENCE_GIVE_UP_NS;
	__atomic_store_n(&of->armed, 1, __ATOMIC_RELEASE);
	/* look first when the vblank is expected */
	due = v->last_ns + period;
	if (due <= now || due > now + period)
		due = now + ATOMIC_OUT_FENCE_RECHECK_NS;
	drm_fence_get(f); /* the timer's */
	hrtimer_start(&of->timer, due);
}

/* IN_FENCE_FD: every plane's fence, waited for before the update is
 * applied (interruptibly: a signal ends the request with -EINTR). */
static int wait_for_in_fences(struct drm_atomic_state *st)
{
	for (uint32_t i = 0; i < st->dev->nplanes; i++) {
		struct drm_plane_state *ps = &st->planes[i];
		int rc;

		if (!ps->in_fence)
			continue;
		rc = drm_fence_wait_flags(ps->in_fence, ATOMIC_IN_FENCE_TIMEOUT_NS, 1);
		if (rc)
			return rc;
		drm_fence_put(ps->in_fence);
		ps->in_fence = NULL;
	}
	return 0;
}

/* ---- the commit ------------------------------------------------------------ */

/* MODE_ID: a blob of the mode while the crtc has one (also while DPMS
 * keeps it off), none otherwise. */
static void crtc_mode_blob_refresh(struct drm_device *dev, struct drm_crtc *c)
{
	if (c->mode_blob) {
		drm_blob_destroy(dev, c->mode_blob);
		c->mode_blob = 0;
	}
	if (DRM_ATOMIC_ENABLE_SEMANTICS ? c->enabled : c->active)
		c->mode_blob = drm_blob_create(dev, &c->mode, sizeof(c->mode));
}

/* The committed value of a property the state keeps, written into the
 * object's list -- what OBJ_GETPROPERTIES reports for it.  Nothing when
 * the property does not exist or is not attached. */
static void props_store(struct drm_object_properties *props, uint32_t prop_id,
			uint64_t val)
{
	if (!prop_id)
		return;
	for (int i = 0; i < props->count; i++) {
		if (props->ids[i] == prop_id) {
			props->values[i] = val;
			return;
		}
	}
}

static uint64_t blob_id_of(const struct drm_blob *b)
{
	return b ? b->id : 0;
}

static void plane_store_props(struct drm_device *dev, struct drm_plane *p)
{
	struct drm_object_properties *props = &p->properties;

	(void)dev;
	if (p->rotation_property)
		props_store(props, p->rotation_property->id, p->rotation);
	if (p->zpos_property)
		props_store(props, p->zpos_property->id, p->zpos);
	if (p->alpha_property)
		props_store(props, p->alpha_property->id, p->alpha);
	if (p->blend_mode_property)
		props_store(props, p->blend_mode_property->id, p->pixel_blend_mode);
	if (p->color_encoding_property)
		props_store(props, p->color_encoding_property->id, p->color_encoding);
	if (p->color_range_property)
		props_store(props, p->color_range_property->id, p->color_range);
	if (p->hotspot_x_property)
		props_store(props, p->hotspot_x_property->id, (uint64_t)(int64_t)p->hot_x);
	if (p->hotspot_y_property)
		props_store(props, p->hotspot_y_property->id, (uint64_t)(int64_t)p->hot_y);
}

static void crtc_store_props(struct drm_device *dev, struct drm_crtc *c)
{
	struct drm_object_properties *props = &c->properties;

	props_store(props, dev->prop_degamma_lut, blob_id_of(c->degamma_lut));
	props_store(props, dev->prop_ctm, blob_id_of(c->ctm));
	props_store(props, dev->prop_gamma_lut, blob_id_of(c->gamma_lut));
	props_store(props, dev->prop_vrr_enabled, (uint64_t)c->vrr_enabled);
}

static void conn_store_props(struct drm_device *dev, struct drm_connector *c)
{
	struct drm_object_properties *props = &c->properties;

	props_store(props, dev->prop_content_type, c->content_type);
	props_store(props, dev->prop_hdr_output_metadata,
		    blob_id_of(c->hdr_output_metadata));
	if (c->scaling_mode_property)
		props_store(props, c->scaling_mode_property->id, c->scaling_mode);
	if (c->colorspace_property)
		props_store(props, c->colorspace_property->id, c->colorspace);
	if (c->max_bpc_property)
		props_store(props, c->max_bpc_property->id, c->max_requested_bpc);
	if (c->broadcast_rgb_property)
		props_store(props, c->broadcast_rgb_property->id, c->broadcast_rgb);
}

#if DRM_LINK_STATUS_SEMANTICS
/* The link status a request set (a bad link GOOD again) is the connector's
 * before the driver commits: the driver's mode set trains the link, and a
 * training that fails marks it BAD again from inside the commit -- which
 * must not then be overwritten with the request's GOOD.  Undone when the
 * commit fails, unless the driver changed it meanwhile. */
static void conn_link_status_store(struct drm_device *dev, struct drm_connector *c,
				   uint64_t val)
{
	__atomic_store_n(&c->link_status, val, __ATOMIC_RELAXED);
	props_store(&c->properties, dev->prop_link_status, val);
}

static void link_status_apply(struct drm_atomic_state *st)
{
	struct drm_device *dev = st->dev;

	for (uint32_t i = 0; i < dev->nconn; i++) {
		struct drm_connector_state *ns = &st->conns[i];

		if (ns->link_status != ns->old_link_status)
			conn_link_status_store(dev, ns->conn, ns->link_status);
	}
}

static void link_status_undo(struct drm_atomic_state *st)
{
	struct drm_device *dev = st->dev;

	for (uint32_t i = 0; i < dev->nconn; i++) {
		struct drm_connector_state *ns = &st->conns[i];
		struct drm_connector *c = ns->conn;

		if (ns->link_status != ns->old_link_status &&
		    __atomic_load_n(&c->link_status, __ATOMIC_RELAXED) == ns->link_status)
			conn_link_status_store(dev, c, ns->old_link_status);
	}
}
#endif

int drm_atomic_commit(struct drm_atomic_state *st)
{
	struct drm_device *dev = st->dev;
	uint32_t i;
	int rc;

	if (!dev->drv->atomic_commit)
		return -ENODEV;
	rc = wait_for_in_fences(st);
	if (rc)
		return rc;
#if DRM_LINK_STATUS_SEMANTICS
	link_status_apply(st);
#endif
	rc = dev->drv->atomic_commit(dev, st);
	if (rc) {
#if DRM_LINK_STATUS_SEMANTICS
		link_status_undo(st);
#endif
		return rc;
	}
	/* The objects take the state. */
	for (i = 0; i < dev->nplanes; i++) {
		struct drm_plane_state *ps = &st->planes[i];
		struct drm_plane *p = ps->plane;
		p->crtc = ps->crtc;
		p->fb_id = ps->fb ? ps->fb->id : 0;
		p->src_x = ps->src_x;
		p->src_y = ps->src_y;
		p->src_w = ps->src_w;
		p->src_h = ps->src_h;
		p->crtc_x = ps->crtc_x;
		p->crtc_y = ps->crtc_y;
		p->crtc_w = ps->crtc_w;
		p->crtc_h = ps->crtc_h;
		p->hot_x = ps->hot_x;
		p->hot_y = ps->hot_y;
		p->rotation = ps->rotation;
		p->zpos = ps->zpos;
		p->normalized_zpos = ps->normalized_zpos;
		p->alpha = ps->alpha;
		p->pixel_blend_mode = ps->pixel_blend_mode;
		p->color_encoding = ps->color_encoding;
		p->color_range = ps->color_range;
		plane_store_props(dev, p);
	}
	for (i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct drm_crtc *c = cs->crtc;
		int was_active = c->active;
		int was_enabled = c->enabled;
		struct drm_plane *prim = drm_crtc_primary(dev, (int)i);
		struct drm_plane *cur = drm_crtc_cursor(dev, (int)i);
		c->active = cs->active;
		c->enabled = cs->mode_valid;
		if (cs->mode_valid)
			c->mode = cs->mode;
#if DRM_ATOMIC_ENABLE_SEMANTICS
		/* the blob follows the mode, not DPMS */
		if (cs->mode_changed || was_enabled != c->enabled ||
		    (c->enabled && !c->mode_blob))
			crtc_mode_blob_refresh(dev, c);
#else
		(void)was_enabled;
		if (cs->mode_changed || cs->active_changed)
			crtc_mode_blob_refresh(dev, c);
#endif
		if (cs->gamma_changed) {
			mm_memcpy(c->gamma, cs->gamma, sizeof(c->gamma));
			c->gamma_identity = cs->gamma_identity;
		}
		drm_property_replace_blob(&c->degamma_lut, cs->degamma_lut);
		drm_property_replace_blob(&c->ctm, cs->ctm);
		drm_property_replace_blob(&c->gamma_lut, cs->gamma_lut);
		c->vrr_enabled = cs->vrr_enabled;
		crtc_store_props(dev, c);
		if (prim && prim->crtc == (int)i) {
			c->fb_id = prim->fb_id;
			c->x = (int)(prim->src_x >> 16);
			c->y = (int)(prim->src_y >> 16);
		} else {
			c->fb_id = 0;
			c->x = c->y = 0;
		}
		if (cur && cur->crtc == (int)i) {
			c->cursor_x = cur->crtc_x;
			c->cursor_y = cur->crtc_y;
		}
		if (was_active != c->active || cs->changed)
			drm_kms_vbl_sync(dev, (int)i);
		if (cs->event && st->fp)
			drm_kms_queue_flip_event(dev, st->fp, (int)i, st->user_data);
		if (cs->out_fence)
			out_fence_arm(cs->out_fence);
	}
	for (i = 0; i < dev->nconn; i++) {
		struct drm_connector_state *ns = &st->conns[i];
		struct drm_connector *c = ns->conn;
#if DRM_ATOMIC_ENABLE_SEMANTICS
		int was = conn_committed_crtc(dev, c);
#endif
		c->crtc_id = ns->crtc >= 0 ? dev->crtc[ns->crtc].id : 0;
#if DRM_ATOMIC_ENABLE_SEMANTICS
		/* DPMS reads what the client set through it; a mode set
		 * (or the connector leaving its crtc) makes it read whether
		 * the crtc runs; a connector on no crtc reads off. */
		if (ns->dpms_changed)
			c->dpms = ns->dpms;
		else if (ns->crtc < 0)
			c->dpms = 3;
		else if (was != ns->crtc ||
			 drm_crtc_state_needs_modeset(&st->crtcs[ns->crtc]))
			c->dpms = dev->crtc[ns->crtc].active ? 0 : 3;
#else
		/* DPMS reads "on" while the connector's crtc runs, else what
		 * the client last set (or off). */
		if (ns->crtc >= 0 && dev->crtc[ns->crtc].active)
			c->dpms = 0;
		else
			c->dpms = (ns->changed && ns->dpms) ? ns->dpms : 3;
#endif
		/* connector i drives encoder i (drm_connector_add) */
		if (i < dev->nenc)
			dev->enc[i].crtc_id = c->crtc_id;
		c->max_requested_bpc = ns->max_requested_bpc;
		c->colorspace = ns->colorspace;
		c->broadcast_rgb = ns->broadcast_rgb;
		c->content_type = ns->content_type;
		c->scaling_mode = ns->scaling_mode;
		drm_property_replace_blob(&c->hdr_output_metadata, ns->hdr_output_metadata);
		conn_store_props(dev, c);
	}
	return 0;
}

/* ---- the older calls, as states ----------------------------------------- */

static int run(struct drm_atomic_state *st)
{
	int rc = drm_atomic_check(st);
	if (rc == 0)
		rc = drm_atomic_commit(st);
	drm_atomic_state_free(st);
	return rc;
}

/* Switch a crtc off in the state: no mode, not active, no plane on it.
 * (Its connectors are the caller's business.) */
static void crtc_state_off(struct drm_atomic_state *st, int crtc)
{
	struct drm_device *dev = st->dev;
	struct drm_crtc_state *cs = &st->crtcs[crtc];

	cs->changed = 1;
	if (cs->active)
		cs->active_changed = 1;
	cs->active = 0;
	if (DRM_ATOMIC_ENABLE_SEMANTICS) {
		if (cs->mode_valid)
			cs->mode_changed = 1;
		cs->mode_valid = 0;
		drm_property_replace_blob(&cs->mode_blob, NULL);
	}
	for (uint32_t i = 0; i < dev->nplanes; i++) {
		struct drm_plane_state *ps = &st->planes[i];
		if (ps->crtc != crtc)
			continue;
		ps->changed = ps->crtc_changed = ps->fb_changed = 1;
		ps->crtc = -1;
		ps->fb = NULL;
		ps->fb_id = 0;
	}
}

/* The connectors a legacy mode set puts on the crtc: the ones named, or
 * -- for the kernel and for a client that names none -- the connector of
 * the crtc's own index, which is how this core pairs them. */
static int crtc_connectors(struct drm_atomic_state *st, struct drm_crtc *crtc,
			   const uint32_t *conn_ids, uint32_t nconn)
{
	struct drm_device *dev = st->dev;
	uint32_t mask = 0;

	for (uint32_t k = 0; k < nconn; k++) {
		struct drm_connector *c = drm_conn_find(dev, conn_ids[k]);
		if (!c)
			return -ENOENT;
		mask |= 1u << (c - dev->conn);
	}
	if (!nconn && (uint32_t)crtc->index < dev->nconn)
		mask = 1u << crtc->index;
	for (uint32_t i = 0; i < dev->nconn; i++) {
		struct drm_connector_state *ns = &st->conns[i];
		if (mask & (1u << i)) {
			ns->crtc = crtc->index;
			ns->changed = 1;
		} else if (ns->crtc == crtc->index) {
			ns->crtc = -1;
			ns->changed = 1;
		}
	}
#if DRM_ATOMIC_ENABLE_SEMANTICS
	/* A crtc whose last connector this took away is switched off, mode
	 * and planes included: an enabled crtc must drive something. */
	for (uint32_t c = 0; c < dev->ncrtc; c++) {
		int has = 0;

		if ((int)c == crtc->index || !st->crtcs[c].mode_valid)
			continue;
		for (uint32_t i = 0; i < dev->nconn; i++)
			if (st->conns[i].crtc == (int)c)
				has = 1;
		if (!has)
			crtc_state_off(st, (int)c);
	}
#endif
	return 0;
}

int drm_atomic_legacy_crtc_set(struct drm_device *dev, struct drm_file *fp,
			       struct drm_crtc *crtc,
			       const struct drm_mode_modeinfo *mode,
			       uint32_t fb_id, int x, int y,
			       const uint32_t *conn_ids, uint32_t nconn)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(dev, fp, 0);
	struct drm_plane *prim = drm_crtc_primary(dev, crtc->index);
	struct drm_plane *cur = drm_crtc_cursor(dev, crtc->index);
	struct drm_crtc_state *cs;
	int rc;

	if (!st)
		return -ENOMEM;
	if (!prim) {
		drm_atomic_state_free(st);
		return -ENODEV;
	}
	st->allow_modeset = 1;
	cs = &st->crtcs[crtc->index];
#if DRM_LINK_STATUS_SEMANTICS
	/* SETCRTC does not say whether it wants a full mode set or only a
	 * new framebuffer, so it may always be one: the links of the crtc's
	 * connectors are set GOOD, and one that was bad is trained again.  A
	 * client that knows nothing of link-status repairs a link that way. */
	for (uint32_t i = 0; i < dev->nconn; i++)
		if (st->conns[i].crtc == crtc->index)
			st->conns[i].link_status = DRM_MODE_LINK_STATUS_GOOD;
#endif
	if (!mode || !fb_id) {
		/* off: the crtc and its mode, its planes, its connectors */
		crtc_state_off(st, crtc->index);
		for (uint32_t i = 0; i < dev->nconn; i++)
			if (st->conns[i].crtc == crtc->index) {
				st->conns[i].crtc = -1;
				st->conns[i].changed = 1;
			}
		return run(st);
	}
	if (x < 0 || y < 0) {
		drm_atomic_state_free(st);
		return -EINVAL;
	}
	cs->changed = 1;
	if (!cs->active)
		cs->active_changed = 1;
	cs->active = 1;
	if (!cs->mode_valid || !drm_mode_equal(&cs->mode, mode))
		cs->mode_changed = 1;
	cs->mode = *mode;
	cs->adjusted_mode = *mode;
	cs->mode_valid = 1;
	rc = drm_atomic_plane_set(st, prim, dev->prop_fb_id, fb_id);
	if (rc == 0)
		rc = drm_atomic_plane_set(st, prim, dev->prop_crtc_id, crtc->id);
	if (rc) {
		drm_atomic_state_free(st);
		return rc;
	}
	struct drm_plane_state *ps = &st->planes[prim->index];
	ps->src_x = (uint32_t)x << 16;
	ps->src_y = (uint32_t)y << 16;
	ps->src_w = (uint32_t)mode->hdisplay << 16;
	ps->src_h = (uint32_t)mode->vdisplay << 16;
	ps->crtc_x = 0;
	ps->crtc_y = 0;
	ps->crtc_w = mode->hdisplay;
	ps->crtc_h = mode->vdisplay;
	ps->geometry_changed = 1;
	/* the cursor follows the crtc it was on */
	if (cur) {
		struct drm_plane_state *cps = &st->planes[cur->index];
		if (cps->fb && cps->crtc != crtc->index) {
			cps->crtc = crtc->index;
			cps->changed = cps->crtc_changed = 1;
		}
	}
	rc = crtc_connectors(st, crtc, conn_ids, nconn);
	if (rc) {
		drm_atomic_state_free(st);
		return rc;
	}
	return run(st);
}

int drm_atomic_legacy_page_flip(struct drm_device *dev, struct drm_file *fp,
				struct drm_crtc *crtc, struct drm_framebuffer *fb,
				int event, uint64_t user_data)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(dev, fp, 0);
	struct drm_plane *prim = drm_crtc_primary(dev, crtc->index);
	int rc;

	if (!st)
		return -ENOMEM;
	if (!prim || !crtc->active) {
		drm_atomic_state_free(st);
		return -EINVAL;
	}
	st->user_data = user_data;
	rc = drm_atomic_plane_set(st, prim, dev->prop_fb_id, fb->id);
	if (rc) {
		drm_atomic_state_free(st);
		return rc;
	}
	struct drm_plane_state *ps = &st->planes[prim->index];
	if (ps->crtc != crtc->index) {
		ps->crtc = crtc->index;
		ps->crtc_changed = 1;
	}
	/* the same rectangle, from the new buffer */
	if (!ps->src_w) {
		ps->src_w = (uint32_t)crtc->mode.hdisplay << 16;
		ps->src_h = (uint32_t)crtc->mode.vdisplay << 16;
		ps->crtc_w = crtc->mode.hdisplay;
		ps->crtc_h = crtc->mode.vdisplay;
	}
	st->crtcs[crtc->index].changed = 1;
	st->crtcs[crtc->index].event = event;
	return run(st);
}

int drm_atomic_legacy_set_plane(struct drm_device *dev, struct drm_file *fp,
				struct drm_plane *plane, uint32_t crtc_id,
				uint32_t fb_id, int32_t crtc_x, int32_t crtc_y,
				uint32_t crtc_w, uint32_t crtc_h, uint32_t src_x,
				uint32_t src_y, uint32_t src_w, uint32_t src_h)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(dev, fp, 0);
	int rc;

	if (!st)
		return -ENOMEM;
	rc = drm_atomic_plane_set(st, plane, dev->prop_fb_id, fb_id);
	if (rc == 0)
		rc = drm_atomic_plane_set(st, plane, dev->prop_crtc_id, fb_id ? crtc_id : 0);
	if (rc) {
		drm_atomic_state_free(st);
		return rc;
	}
	struct drm_plane_state *ps = &st->planes[plane->index];
	ps->src_x = src_x;
	ps->src_y = src_y;
	ps->src_w = src_w;
	ps->src_h = src_h;
	ps->crtc_x = crtc_x;
	ps->crtc_y = crtc_y;
	ps->crtc_w = crtc_w;
	ps->crtc_h = crtc_h;
	ps->geometry_changed = 1;
	return run(st);
}

/* Can `o' be shown as a w x h ARGB8888 cursor?  The driver says, when it
 * has a say; otherwise a plain buffer must hold the image, and any other
 * kind of object -- a surface, whose size is that of its backing store --
 * is left to the driver's plane check. */
static int cursor_obj_check(struct drm_device *dev, struct drm_gem_object *o,
			    uint32_t w, uint32_t h)
{
	if (!w || !h)
		return -EINVAL;
#if DRM_CURSOR_OBJ_CHECK
	if (dev->drv->cursor_obj_check)
		return dev->drv->cursor_obj_check(dev, o, w, h);
	if (o->kind != DRM_GEM_BO)
		return 0;
#else
	(void)dev;
#endif
	if ((uint64_t)w * h * 4 > o->size)
		return -EINVAL;
	return 0;
}

int drm_atomic_legacy_cursor(struct drm_device *dev, struct drm_file *fp,
			     struct drm_crtc *crtc, uint32_t flags,
			     struct drm_gem_object *o, uint32_t w, uint32_t h,
			     int32_t hot_x, int32_t hot_y, int x, int y)
{
	struct drm_plane *cur = drm_crtc_cursor(dev, crtc->index);
	struct drm_atomic_state *st;
	int rc = 0;

	if (!cur)
		return -ENODEV;
	st = drm_atomic_state_alloc(dev, fp, 0);
	if (!st)
		return -ENOMEM;
	struct drm_plane_state *ps = &st->planes[cur->index];
	if (flags & DRM_MODE_CURSOR_BO) {
		uint32_t fb_id = 0;
		if (o) {
			rc = cursor_obj_check(dev, o, w, h);
			if (rc) {
				drm_atomic_state_free(st);
				return rc;
			}
			rc = drm_kms_fb_add_internal(dev, o, w, h, w * 4,
						     DRM_FORMAT_ARGB8888, &fb_id);
			if (rc) {
				drm_atomic_state_free(st);
				return rc;
			}
		}
		ps->changed = ps->fb_changed = 1;
		ps->fb_id = fb_id;
		ps->fb = fb_id ? drm_fb_lookup(dev, fb_id) : NULL;
		if (fb_id) {
			if (ps->crtc != crtc->index)
				ps->crtc_changed = 1;
			ps->crtc = crtc->index;
			ps->src_x = ps->src_y = 0;
			ps->src_w = w << 16;
			ps->src_h = h << 16;
			ps->crtc_w = w;
			ps->crtc_h = h;
			ps->hot_x = hot_x;
			ps->hot_y = hot_y;
		} else {
			if (ps->crtc >= 0)
				ps->crtc_changed = 1;
			ps->crtc = -1;
		}
		ps->geometry_changed = 1;
	}
	if (flags & DRM_MODE_CURSOR_MOVE) {
		ps->changed = ps->geometry_changed = 1;
		ps->crtc_x = x;
		ps->crtc_y = y;
	}
	uint32_t old_fb = crtc->cursor_fb_id;
	uint32_t new_fb = ps->fb_id;
	st->crtcs[crtc->index].changed = 1;
	rc = run(st);
	if (rc) {
		if ((flags & DRM_MODE_CURSOR_BO) && new_fb && new_fb != old_fb)
			drm_kms_fb_remove_internal(dev, new_fb);
		return rc;
	}
	if (flags & DRM_MODE_CURSOR_BO) {
		crtc->cursor_fb_id = new_fb;
		if (old_fb && old_fb != new_fb)
			drm_kms_fb_remove_internal(dev, old_fb);
		crtc->cursor_handle_w = w;
		crtc->cursor_handle_h = h;
	}
	return 0;
}

int drm_atomic_legacy_dpms(struct drm_device *dev, struct drm_file *fp,
			   struct drm_connector *c, int mode)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(dev, fp, 0);
	int rc;

	if (!st)
		return -ENOMEM;
	st->allow_modeset = 1;
	rc = drm_atomic_conn_set(st, c, dev->prop_dpms, (uint64_t)mode);
	if (rc) {
		drm_atomic_state_free(st);
		return rc;
	}
	return run(st);
}

/*
 * SETGAMMA on an atomic driver: the table becomes the crtc's GAMMA_LUT (a
 * blob of its 256 entries, so that the property reads back what is shown),
 * and DEGAMMA_LUT / CTM are cleared, the way the table is the whole colour
 * pipeline for a client of the older interface (drm_color_mgmt.c).  The
 * drivers program the 256-entry table of the state, which is applied
 * whatever happens to the blob: without room for it GAMMA_LUT simply reads
 * as before.
 */
int drm_atomic_legacy_gamma(struct drm_device *dev, struct drm_file *fp,
			    struct drm_crtc *crtc, const uint16_t *r,
			    const uint16_t *g, const uint16_t *b, uint32_t size)
{
	struct drm_atomic_state *st;
	struct drm_crtc_state *cs;

	if (size < 2 || size > 256)
		return -EINVAL;
	st = drm_atomic_state_alloc(dev, fp, 0);
	if (!st)
		return -ENOMEM;
	cs = &st->crtcs[crtc->index];
	if (size == 256) {
		mm_memcpy(cs->gamma[0], r, 512);
		mm_memcpy(cs->gamma[1], g, 512);
		mm_memcpy(cs->gamma[2], b, 512);
	} else {
		/* a shorter legacy table, resampled onto the 256 entries the
		 * drivers program */
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t k = (uint32_t)(((uint64_t)i * (size - 1) + 127) / 255);
			cs->gamma[0][i] = r[k];
			cs->gamma[1][i] = g[k];
			cs->gamma[2][i] = b[k];
		}
	}
	cs->gamma_identity = gamma_is_identity(cs);
	cs->gamma_changed = 1;
	cs->changed = 1;
	if (dev->prop_gamma_lut || dev->prop_degamma_lut) {
		/* the crtc's list as clients see it, core properties on */
		(void)drm_mode_object_props_of(dev, DRM_MODE_OBJECT_CRTC, crtc);
		(void)drm_crtc_legacy_gamma_set_state(dev, cs, r, g, b, size);
	}
	return run(st);
}

int drm_atomic_legacy_set_property(struct drm_device *dev, struct drm_file *fp,
				   uint32_t obj_type, uint32_t obj_id,
				   uint32_t prop, uint64_t val)
{
	struct drm_atomic_state *st;
	int rc;

#if DRM_ATOMIC_ENABLE_SEMANTICS
	/* DPMS to the level the connector is at already changes nothing,
	 * and is not a request (standby and suspend are off). */
	if (obj_type == DRM_MODE_OBJECT_CONNECTOR && prop && prop == dev->prop_dpms) {
		struct drm_connector *c = drm_conn_find(dev, obj_id);

		if (c && c->dpms == (val == 0 ? 0 : 3))
			return 0;
	}
#endif
	st = drm_atomic_state_alloc(dev, fp, 0);
	if (!st)
		return -ENOMEM;
	st->allow_modeset = 1;
	switch (obj_type) {
	case DRM_MODE_OBJECT_CONNECTOR: {
		struct drm_connector *c = drm_conn_find(dev, obj_id);
		rc = c ? drm_atomic_conn_set(st, c, prop, val) : -ENOENT;
		break;
	}
	case DRM_MODE_OBJECT_CRTC: {
		struct drm_crtc *c = drm_crtc_find(dev, obj_id);
		rc = c ? drm_atomic_crtc_set(st, c, prop, val) : -ENOENT;
		break;
	}
	case DRM_MODE_OBJECT_PLANE: {
		struct drm_plane *p = drm_plane_find(dev, obj_id);
		rc = p ? drm_atomic_plane_set(st, p, prop, val) : -ENOENT;
		break;
	}
	default:
		rc = -ENOENT;
		break;
	}
	if (rc) {
		drm_atomic_state_free(st);
		return rc;
	}
	return run(st);
}

/* ---- after a resume ------------------------------------------------------- */

int drm_atomic_replay(struct drm_device *dev)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(dev, NULL, 0);
	int rc;

	if (!st)
		return -ENOMEM;
	st->allow_modeset = 1;
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		if (!cs->active)
			continue;
		cs->changed = cs->mode_changed = cs->gamma_changed = 1;
	}
	for (uint32_t i = 0; i < dev->nplanes; i++) {
		struct drm_plane_state *ps = &st->planes[i];
		if (ps->crtc >= 0)
			ps->changed = ps->fb_changed = ps->geometry_changed = 1;
	}
	for (uint32_t i = 0; i < dev->nconn; i++)
		st->conns[i].changed = 1;
	rc = drm_atomic_check(st);
	if (rc == 0)
		rc = drm_atomic_commit(st);
	drm_atomic_state_free(st);
	return rc;
}

/* ---- the ATOMIC ioctl ----------------------------------------------------- */

#define ATOMIC_MAX_OBJS 64
#define ATOMIC_MAX_PROPS 1024

/*
 * Before the check: an out-fence for every crtc that asked for one, its
 * descriptor installed and written to the client's pointer.  Nothing for a
 * TEST_ONLY request (the pointers already read -1).
 */
static int prepare_signaling(struct drm_atomic_state *st, uint32_t flags)
{
	struct drm_device *dev = st->dev;

	if (flags & DRM_MODE_ATOMIC_TEST_ONLY)
		return 0;
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct drm_fence *f;
		int32_t fd;

		if (!cs->out_fence_ptr)
			continue;
		f = out_fence_create(dev, (int)i);
		if (!f)
			return -ENOMEM;
		cs->out_fence = f;
		fd = drm_sync_file_install(f, NULL, 1);
		if (fd < 0)
			return fd;
		cs->out_fence_fd = fd;
		if (copy_to_user((void *)(uintptr_t)cs->out_fence_ptr, &fd, sizeof(fd)) != 0)
			return -EFAULT;
	}
	return 0;
}

/*
 * After the request: on success the out-fences were armed by the commit
 * and their descriptors stay with the client.  On failure every one is
 * cancelled -- signalled with an error, so that nothing that got hold of
 * the descriptor meanwhile waits for ever -- its descriptor closed again
 * and the client's pointer set back to -1.
 */
static void complete_signaling(struct drm_atomic_state *st, int ok)
{
	struct drm_device *dev = st->dev;

	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];

		if (!cs->out_fence)
			continue;
		if (!ok) {
			int32_t none = -1;

			drm_fence_set_error(cs->out_fence, ATOMIC_OUT_FENCE_CANCELED);
			drm_fence_signal(cs->out_fence);
			if (cs->out_fence_fd >= 0)
				(void)sys_close((uint64_t)cs->out_fence_fd);
			if (cs->out_fence_ptr &&
			    copy_to_user((void *)(uintptr_t)cs->out_fence_ptr, &none,
					 sizeof(none)) != 0) {
				/* the client unmapped it meanwhile: its loss */
			}
		}
		cs->out_fence_fd = -1;
		drm_fence_put(cs->out_fence);
		cs->out_fence = NULL;
	}
}

/* The crtc index of a connector or plane before and after the request,
 * as a mask (the crtcs it pulls into the request). */
static uint32_t crtc_bit(int crtc)
{
	return crtc >= 0 && crtc < DRM_MAX_CRTCS ? 1u << crtc : 0;
}

long drm_atomic_ioctl(struct drm_device *dev, struct drm_file *fp,
		      struct drm_mode_atomic *a)
{
	uint32_t nobj = a->count_objs;
	uint32_t objs[ATOMIC_MAX_OBJS], objcnt[ATOMIC_MAX_OBJS];
	struct drm_atomic_state *st;
	uint32_t pidx = 0;
	int rc = 0;

	if (a->flags & ~DRM_MODE_ATOMIC_FLAGS)
		return -EINVAL;
	if ((a->flags & DRM_MODE_ATOMIC_TEST_ONLY) && (a->flags & DRM_MODE_PAGE_FLIP_EVENT))
		return -EINVAL;
	if (a->flags & DRM_MODE_PAGE_FLIP_ASYNC)
		return -EINVAL; /* flips are shown at a vblank here */
	if (a->reserved)
		return -EINVAL;
	if (nobj > ATOMIC_MAX_OBJS)
		return -EINVAL;
	if (nobj) {
		if (!validate_user_ptr(a->objs_ptr, nobj * 4) ||
		    !validate_user_ptr(a->count_props_ptr, nobj * 4))
			return -EFAULT;
		if (copy_from_user(objs, (void *)(uintptr_t)a->objs_ptr, nobj * 4) != 0 ||
		    copy_from_user(objcnt, (void *)(uintptr_t)a->count_props_ptr, nobj * 4) != 0)
			return -EFAULT;
	}
	uint32_t total = 0;
	for (uint32_t i = 0; i < nobj; i++) {
		if (objcnt[i] > ATOMIC_MAX_PROPS)
			return -EINVAL;
		total += objcnt[i];
		if (total > ATOMIC_MAX_PROPS)
			return -EINVAL;
	}
	if (total && (!validate_user_ptr(a->props_ptr, total * 4) ||
		      !validate_user_ptr(a->prop_values_ptr, total * 8)))
		return -EFAULT;
	st = drm_atomic_state_alloc(dev, fp, a->flags);
	if (!st)
		return -ENOMEM;
	st->allow_modeset = !!(a->flags & DRM_MODE_ATOMIC_ALLOW_MODESET);
	st->user_data = a->user_data;
	/* Which crtcs the request names -- directly, or through a plane or
	 * connector that is on one before or after it: those get the
	 * event. */
	uint32_t named_crtcs = 0;
	for (uint32_t i = 0; i < nobj && rc == 0; i++) {
		uint32_t type = 0;
		void *obj = drm_mode_object_find(dev, objs[i], DRM_MODE_OBJECT_ANY, &type);
		struct drm_object_properties *props;

		if (!obj) {
			rc = -ENOENT;
			break;
		}
		/* only crtcs, planes and connectors carry properties */
		props = drm_mode_object_props_of(dev, type, obj);
		if (!props) {
			rc = -ENOENT;
			break;
		}
		if (type == DRM_MODE_OBJECT_CRTC) {
			named_crtcs |= crtc_bit(((struct drm_crtc *)obj)->index);
		} else if (type == DRM_MODE_OBJECT_PLANE) {
			named_crtcs |= crtc_bit(st->planes[((struct drm_plane *)obj)->index].crtc);
		} else {
			struct drm_connector *cn = obj;
			named_crtcs |= crtc_bit(st->conns[cn - dev->conn].crtc);
		}
		for (uint32_t j = 0; j < objcnt[i]; j++, pidx++) {
			struct drm_prop *prop;
			uint32_t pid;
			uint64_t val;
			if (copy_from_user(&pid, (void *)(uintptr_t)(a->props_ptr + pidx * 4), 4) != 0 ||
			    copy_from_user(&val, (void *)(uintptr_t)(a->prop_values_ptr + pidx * 8), 8) != 0) {
				rc = -EFAULT;
				break;
			}
			/* the object must carry the property */
			prop = drm_mode_obj_find_prop_id(dev, props, pid);
			if (!prop) {
				rc = -ENOENT;
				break;
			}
			rc = drm_atomic_set_property(st, type, obj, prop, val);
			if (rc)
				break;
		}
		if (type == DRM_MODE_OBJECT_PLANE)
			named_crtcs |= crtc_bit(st->planes[((struct drm_plane *)obj)->index].crtc);
		else if (type == DRM_MODE_OBJECT_CONNECTOR)
			named_crtcs |= crtc_bit(st->conns[(struct drm_connector *)obj - dev->conn].crtc);
	}
	if (rc == 0)
		rc = prepare_signaling(st, a->flags);
	if (rc == 0 && (a->flags & DRM_MODE_PAGE_FLIP_EVENT)) {
		/* an event nothing would ever send is refused here */
		if (!named_crtcs)
			rc = -EINVAL;
		for (uint32_t i = 0; i < dev->ncrtc; i++)
			if (named_crtcs & (1u << i))
				st->crtcs[i].event = 1;
	}
	if (rc == 0)
		rc = drm_atomic_check(st);
	if (rc == 0 && !(a->flags & DRM_MODE_ATOMIC_TEST_ONLY))
		rc = drm_atomic_commit(st);
	complete_signaling(st, rc == 0);
	drm_atomic_state_free(st);
	return rc;
}
