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
// The older calls -- SETCRTC, PAGE_FLIP, SETPLANE, CURSOR, DPMS, SETGAMMA
// and the kernel console's mode set -- are translated into such states
// here for a driver that has atomic entry points, so that driver has one
// path to program the hardware through, not six.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

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
	}
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc *c = &dev->crtc[i];
		struct drm_crtc_state *cs = &st->crtcs[i];
		cs->crtc = c;
		cs->active = c->active;
		cs->mode_valid = c->active;
		cs->mode = c->mode;
		cs->adjusted_mode = c->mode;
		mm_memcpy(cs->gamma, c->gamma, sizeof(cs->gamma));
		cs->gamma_identity = c->gamma_identity;
	}
	for (uint32_t i = 0; i < dev->nconn; i++) {
		struct drm_connector *c = &dev->conn[i];
		struct drm_connector_state *ns = &st->conns[i];
		ns->conn = c;
		ns->crtc = -1;
		ns->dpms = c->dpms;
		if (c->crtc_id) {
			struct drm_crtc *cr = drm_crtc_find(dev, c->crtc_id);
			if (cr)
				ns->crtc = cr->index;
		}
	}
	return st;
}

void drm_atomic_state_free(struct drm_atomic_state *st)
{
	if (st)
		kfree(st);
}

static int crtc_index_of(struct drm_device *dev, uint64_t id)
{
	if (!id)
		return -1;
	struct drm_crtc *c = drm_crtc_find(dev, (uint32_t)id);
	return c ? c->index : -2; /* -2: no such crtc */
}

/* ---- property writes ----------------------------------------------------- */

int drm_atomic_plane_set(struct drm_atomic_state *st, struct drm_plane *p,
			 uint32_t prop, uint64_t val)
{
	struct drm_device *dev = st->dev;
	struct drm_plane_state *ps = &st->planes[p->index];

	ps->changed = 1;
	if (prop == dev->prop_fb_id) {
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
	if (prop == dev->prop_crtc_id) {
		int idx = crtc_index_of(dev, val);
		if (idx == -2)
			return -ENOENT;
		if (ps->crtc != idx)
			ps->crtc_changed = 1;
		ps->crtc = idx;
		return 0;
	}
	ps->geometry_changed = 1;
	if (prop == dev->prop_src_x)
		ps->src_x = (uint32_t)val;
	else if (prop == dev->prop_src_y)
		ps->src_y = (uint32_t)val;
	else if (prop == dev->prop_src_w)
		ps->src_w = (uint32_t)val;
	else if (prop == dev->prop_src_h)
		ps->src_h = (uint32_t)val;
	else if (prop == dev->prop_crtc_x)
		ps->crtc_x = (int32_t)val;
	else if (prop == dev->prop_crtc_y)
		ps->crtc_y = (int32_t)val;
	else if (prop == dev->prop_crtc_w)
		ps->crtc_w = (uint32_t)val;
	else if (prop == dev->prop_crtc_h)
		ps->crtc_h = (uint32_t)val;
	else
		return -EINVAL; /* immutable or unknown */
	return 0;
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

int drm_atomic_crtc_set(struct drm_atomic_state *st, struct drm_crtc *c,
			uint32_t prop, uint64_t val)
{
	struct drm_device *dev = st->dev;
	struct drm_crtc_state *cs = &st->crtcs[c->index];

	cs->changed = 1;
	if (prop == dev->prop_active) {
		int active = val ? 1 : 0;
		if (cs->active != active)
			cs->active_changed = 1;
		cs->active = active;
		return 0;
	}
	if (prop == dev->prop_mode_id) {
		if (!val) {
			if (cs->mode_valid)
				cs->mode_changed = 1;
			cs->mode_valid = 0;
			return 0;
		}
		struct drm_blob *b = drm_blob_find(dev, (uint32_t)val);
		if (!b)
			return -ENOENT;
		if (b->length != sizeof(struct drm_mode_modeinfo))
			return -EINVAL;
		struct drm_mode_modeinfo m;
		mm_memcpy(&m, b->data, sizeof(m));
		if (!cs->mode_valid || !drm_mode_equal(&cs->mode, &m))
			cs->mode_changed = 1;
		cs->mode = m;
		cs->adjusted_mode = m;
		cs->mode_valid = 1;
		return 0;
	}
	if (prop == dev->prop_gamma_lut && dev->prop_gamma_lut) {
		if (!val) {
			gamma_identity(cs);
			cs->gamma_identity = 1;
			cs->gamma_changed = 1;
			return 0;
		}
		struct drm_blob *b = drm_blob_find(dev, (uint32_t)val);
		if (!b)
			return -ENOENT;
		uint32_t n = b->length / sizeof(struct drm_color_lut);
		if (n < 2 || b->length % sizeof(struct drm_color_lut))
			return -EINVAL;
		gamma_from_lut(cs, b->data, n);
		cs->gamma_identity = gamma_is_identity(cs);
		cs->gamma_changed = 1;
		return 0;
	}
	return -EINVAL; /* immutable or unknown */
}

int drm_atomic_conn_set(struct drm_atomic_state *st, struct drm_connector *c,
			uint32_t prop, uint64_t val)
{
	struct drm_device *dev = st->dev;
	int idx = (int)(c - dev->conn);
	struct drm_connector_state *ns = &st->conns[idx];

	ns->changed = 1;
	if (prop == dev->prop_crtc_id) {
		int ci = crtc_index_of(dev, val);
		if (ci == -2)
			return -ENOENT;
		ns->crtc = ci;
		return 0;
	}
	if (prop == dev->prop_dpms) {
		/* The older knob on an atomic driver: on = the connector's
		 * crtc active, anything else = off.  The value itself is
		 * remembered for GETCONNECTOR. */
		int on = (val == 0);
		ns->dpms = (int)val;
		if (ns->crtc >= 0) {
			struct drm_crtc_state *cs = &st->crtcs[ns->crtc];
			cs->changed = 1;
			if (cs->active != on)
				cs->active_changed = 1;
			cs->active = on;
		}
		return 0;
	}
	if (prop == dev->prop_edid || prop == dev->prop_link_status ||
	    prop == dev->prop_non_desktop)
		return -EINVAL; /* immutable */
	return -EINVAL;
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

int drm_atomic_check(struct drm_atomic_state *st)
{
	struct drm_device *dev = st->dev;
	uint32_t i;

	/* Connectors: which crtc each is on, and so which crtcs have one. */
	for (i = 0; i < dev->ncrtc; i++)
		st->crtcs[i].connector_mask = 0;
	for (i = 0; i < dev->nconn; i++) {
		struct drm_connector_state *ns = &st->conns[i];
		int was = -1;
		if (ns->conn->crtc_id) {
			struct drm_crtc *cr = drm_crtc_find(dev, ns->conn->crtc_id);
			if (cr)
				was = cr->index;
		}
		if (ns->crtc >= (int)dev->ncrtc)
			return -EINVAL;
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
	}
	/* CRTCs. */
	for (i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		if (cs->active && !cs->mode_valid)
			return -EINVAL;
		if (cs->active && !cs->connector_mask)
			return -EINVAL;
		if (cs->mode_valid) {
			const struct drm_mode_modeinfo *m = &cs->mode;
			if (!m->hdisplay || !m->vdisplay || !m->htotal || !m->vtotal ||
			    !m->clock || m->hdisplay > m->htotal ||
			    m->vdisplay > m->vtotal ||
			    m->hdisplay > dev->max_width || m->vdisplay > dev->max_height)
				return -EINVAL;
		}
		if (cs->mode_changed || cs->active_changed || cs->connectors_changed) {
			if (!st->allow_modeset)
				return -EINVAL;
			cs->changed = 1;
		}
	}
	/* Planes. */
	for (i = 0; i < dev->nplanes; i++) {
		struct drm_plane_state *ps = &st->planes[i];
		struct drm_plane *p = ps->plane;
		if (ps->crtc >= (int)dev->ncrtc)
			return -EINVAL;
		if (ps->crtc >= 0 && !(p->possible_crtcs & (1u << ps->crtc)))
			return -EINVAL;
		/* a framebuffer needs a crtc and a crtc a framebuffer */
		if ((ps->fb != NULL) != (ps->crtc >= 0))
			return -EINVAL;
		if (ps->changed && ps->crtc >= 0)
			st->crtcs[ps->crtc].changed = 1;
		if (ps->crtc_changed && p->crtc >= 0 && p->crtc != ps->crtc)
			st->crtcs[p->crtc].changed = 1;
		if (!ps->fb)
			continue;
		struct drm_crtc_state *cs = &st->crtcs[ps->crtc];
		if (!cs->active)
			return -EINVAL;
		if (!plane_takes(p, ps->fb->format, ps->fb->modifier))
			return -EINVAL;
		/* the source rectangle inside the framebuffer (16.16) */
		if (!ps->src_w || !ps->src_h || !ps->crtc_w || !ps->crtc_h)
			return -EINVAL;
		if ((uint64_t)ps->src_x + ps->src_w > ((uint64_t)ps->fb->width << 16) ||
		    (uint64_t)ps->src_y + ps->src_h > ((uint64_t)ps->fb->height << 16))
			return -ENOSPC;
		/* no scaling inside a plane: the source and the destination
		 * are the same size (the pipe scaler is a crtc matter) */
		if ((ps->src_w >> 16) != ps->crtc_w || (ps->src_h >> 16) != ps->crtc_h)
			return -EINVAL;
		if (p->type == DRM_PLANE_TYPE_CURSOR && dev->drv->cursor_w &&
		    (ps->crtc_w > dev->drv->cursor_w || ps->crtc_h > dev->drv->cursor_h))
			return -EINVAL;
	}
	if (dev->drv->atomic_check)
		return dev->drv->atomic_check(dev, st);
	return 0;
}

/* ---- the commit ------------------------------------------------------------ */

static void crtc_mode_blob_refresh(struct drm_device *dev, struct drm_crtc *c)
{
	if (c->mode_blob) {
		drm_blob_destroy(dev, c->mode_blob);
		c->mode_blob = 0;
	}
	if (c->active)
		c->mode_blob = drm_blob_create(dev, &c->mode, sizeof(c->mode));
}

int drm_atomic_commit(struct drm_atomic_state *st)
{
	struct drm_device *dev = st->dev;
	uint32_t i;
	int rc;

	if (!dev->drv->atomic_commit)
		return -ENODEV;
	rc = dev->drv->atomic_commit(dev, st);
	if (rc)
		return rc;
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
	}
	for (i = 0; i < dev->ncrtc; i++) {
		struct drm_crtc_state *cs = &st->crtcs[i];
		struct drm_crtc *c = cs->crtc;
		int was_active = c->active;
		struct drm_plane *prim = drm_crtc_primary(dev, (int)i);
		struct drm_plane *cur = drm_crtc_cursor(dev, (int)i);
		c->active = cs->active;
		if (cs->mode_valid)
			c->mode = cs->mode;
		if (cs->mode_changed || cs->active_changed)
			crtc_mode_blob_refresh(dev, c);
		if (cs->gamma_changed) {
			mm_memcpy(c->gamma, cs->gamma, sizeof(c->gamma));
			c->gamma_identity = cs->gamma_identity;
		}
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
	}
	for (i = 0; i < dev->nconn; i++) {
		struct drm_connector_state *ns = &st->conns[i];
		struct drm_connector *c = ns->conn;
		c->crtc_id = ns->crtc >= 0 ? dev->crtc[ns->crtc].id : 0;
		/* DPMS reads "on" while the connector's crtc runs, else what
		 * the client last set (or off). */
		if (ns->crtc >= 0 && dev->crtc[ns->crtc].active)
			c->dpms = 0;
		else
			c->dpms = (ns->changed && ns->dpms) ? ns->dpms : 3;
		/* connector i drives encoder i (drm_connector_add) */
		if (i < dev->nenc)
			dev->enc[i].crtc_id = c->crtc_id;
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
	if (!mode || !fb_id) {
		/* off: the crtc, its planes, its connectors */
		cs->changed = 1;
		if (cs->active)
			cs->active_changed = 1;
		cs->active = 0;
		for (uint32_t i = 0; i < dev->nplanes; i++) {
			struct drm_plane_state *ps = &st->planes[i];
			if (ps->crtc != crtc->index)
				continue;
			ps->changed = ps->crtc_changed = ps->fb_changed = 1;
			ps->crtc = -1;
			ps->fb = NULL;
			ps->fb_id = 0;
		}
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
			if (!w || !h || (uint64_t)w * h * 4 > o->size) {
				drm_atomic_state_free(st);
				return -EINVAL;
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

int drm_atomic_legacy_gamma(struct drm_device *dev, struct drm_file *fp,
			    struct drm_crtc *crtc, const uint16_t *r,
			    const uint16_t *g, const uint16_t *b)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(dev, fp, 0);
	struct drm_crtc_state *cs;

	if (!st)
		return -ENOMEM;
	cs = &st->crtcs[crtc->index];
	mm_memcpy(cs->gamma[0], r, 512);
	mm_memcpy(cs->gamma[1], g, 512);
	mm_memcpy(cs->gamma[2], b, 512);
	cs->gamma_identity = gamma_is_identity(cs);
	cs->gamma_changed = 1;
	cs->changed = 1;
	return run(st);
}

int drm_atomic_legacy_set_property(struct drm_device *dev, struct drm_file *fp,
				   uint32_t obj_type, uint32_t obj_id,
				   uint32_t prop, uint64_t val)
{
	struct drm_atomic_state *st = drm_atomic_state_alloc(dev, fp, 0);
	int rc;

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

long drm_atomic_ioctl(struct drm_device *dev, struct drm_file *fp,
		      struct drm_mode_atomic *a)
{
	uint32_t nobj = a->count_objs;
	uint32_t objs[ATOMIC_MAX_OBJS], objcnt[ATOMIC_MAX_OBJS];
	struct drm_atomic_state *st;
	uint32_t pidx = 0;
	int rc = 0;

	if (a->flags & ~(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK |
			 DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT |
			 DRM_MODE_PAGE_FLIP_ASYNC))
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
	/* Which crtcs the request names (directly, or through a plane or
	 * connector of theirs): those get the event. */
	uint32_t named_crtcs = 0;
	for (uint32_t i = 0; i < nobj && rc == 0; i++) {
		struct drm_crtc *cr = drm_crtc_find(dev, objs[i]);
		struct drm_plane *pl = cr ? NULL : drm_plane_find(dev, objs[i]);
		struct drm_connector *cn = (cr || pl) ? NULL : drm_conn_find(dev, objs[i]);
		if (!cr && !pl && !cn) {
			rc = -ENOENT;
			break;
		}
		if (cr)
			named_crtcs |= 1u << cr->index;
		if (pl && st->planes[pl->index].crtc >= 0)
			named_crtcs |= 1u << st->planes[pl->index].crtc;
		for (uint32_t j = 0; j < objcnt[i]; j++, pidx++) {
			uint32_t pid;
			uint64_t val;
			if (copy_from_user(&pid, (void *)(uintptr_t)(a->props_ptr + pidx * 4), 4) != 0 ||
			    copy_from_user(&val, (void *)(uintptr_t)(a->prop_values_ptr + pidx * 8), 8) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cr)
				rc = drm_atomic_crtc_set(st, cr, pid, val);
			else if (pl)
				rc = drm_atomic_plane_set(st, pl, pid, val);
			else
				rc = drm_atomic_conn_set(st, cn, pid, val);
			if (rc)
				break;
		}
		if (pl && st->planes[pl->index].crtc >= 0)
			named_crtcs |= 1u << st->planes[pl->index].crtc;
	}
	if (rc == 0)
		rc = drm_atomic_check(st);
	if (rc == 0 && !(a->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
		if (a->flags & DRM_MODE_PAGE_FLIP_EVENT) {
			for (uint32_t i = 0; i < dev->ncrtc; i++)
				if ((named_crtcs & (1u << i)) && st->crtcs[i].active)
					st->crtcs[i].event = 1;
		}
		rc = drm_atomic_commit(st);
	}
	drm_atomic_state_free(st);
	return rc;
}
