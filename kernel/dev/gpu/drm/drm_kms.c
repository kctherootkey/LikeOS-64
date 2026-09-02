// LikeOS-64 -- display-manager core: mode setting.
//
// The mode objects (connectors, encoders, crtcs, planes, properties,
// blobs, framebuffers), the legacy and atomic mode-setting ioctls, dumb
// buffers, cursors, page flips, dirty rectangles, and the vblank counter
// with its events.  What touches hardware goes through drm_driver.
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/net/net.h>
#include <kernel/io/console.h>

/* ---- objects and properties ------------------------------------------ */

static uint32_t mode_id_alloc(struct drm_device *dev)
{
	return dev->next_mode_id++;
}

static struct drm_prop *prop_add(struct drm_device *dev, const char *name,
				 uint32_t flags)
{
	if (dev->nprops >= DRM_MAX_PROPS)
		return NULL;
	struct drm_prop *p = &dev->props[dev->nprops++];
	mm_memset(p, 0, sizeof(*p));
	p->id = mode_id_alloc(dev);
	p->flags = flags;
	for (int i = 0; name[i] && i < 31; i++)
		p->name[i] = name[i];
	return p;
}

static void prop_enum(struct drm_prop *p, uint64_t value, const char *name)
{
	if (p->nenums >= 8)
		return;
	struct drm_mode_property_enum *e = &p->enums[p->nenums++];
	e->value = value;
	mm_memset(e->name, 0, sizeof(e->name));
	for (int i = 0; name[i] && i < 31; i++)
		e->name[i] = name[i];
}

static struct drm_prop *prop_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nprops; i++)
		if (dev->props[i].id == id)
			return &dev->props[i];
	return NULL;
}

static uint32_t blob_create(struct drm_device *dev, const void *data,
			    uint32_t length)
{
	for (int i = 0; i < DRM_MAX_BLOBS; i++) {
		if (dev->blobs[i].in_use)
			continue;
		void *d = kalloc(length ? length : 1);
		if (!d)
			return 0;
		if (data)
			mm_memcpy(d, data, length);
		dev->blobs[i].in_use = 1;
		dev->blobs[i].id = mode_id_alloc(dev);
		dev->blobs[i].data = d;
		dev->blobs[i].length = length;
		return dev->blobs[i].id;
	}
	return 0;
}

static struct drm_blob *blob_find(struct drm_device *dev, uint32_t id)
{
	for (int i = 0; i < DRM_MAX_BLOBS; i++)
		if (dev->blobs[i].in_use && dev->blobs[i].id == id)
			return &dev->blobs[i];
	return NULL;
}

static void blob_destroy(struct drm_device *dev, uint32_t id)
{
	struct drm_blob *b = blob_find(dev, id);
	if (b) {
		kfree(b->data);
		b->in_use = 0;
	}
}

static struct drm_crtc *crtc_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->ncrtc; i++)
		if (dev->crtc[i].id == id)
			return &dev->crtc[i];
	return NULL;
}

static struct drm_connector *conn_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nconn; i++)
		if (dev->conn[i].id == id)
			return &dev->conn[i];
	return NULL;
}

static struct drm_encoder *enc_find(struct drm_device *dev, uint32_t id)
{
	for (uint32_t i = 0; i < dev->nenc; i++)
		if (dev->enc[i].id == id)
			return &dev->enc[i];
	return NULL;
}

/* Planes are numbered from the crtc: primary = crtc.primary_plane_id. */
static struct drm_crtc *plane_crtc(struct drm_device *dev, uint32_t plane_id,
				   int *is_cursor)
{
	for (uint32_t i = 0; i < dev->ncrtc; i++) {
		if (dev->crtc[i].primary_plane_id == plane_id) {
			*is_cursor = 0;
			return &dev->crtc[i];
		}
		if (dev->crtc[i].cursor_plane_id == plane_id) {
			*is_cursor = 1;
			return &dev->crtc[i];
		}
	}
	return NULL;
}

struct drm_framebuffer *drm_fb_lookup(struct drm_device *dev, uint32_t id)
{
	if (!id)
		return NULL;
	for (int i = 0; i < DRM_MAX_FBS; i++)
		if (dev->fbs[i].id == id)
			return &dev->fbs[i];
	return NULL;
}

struct drm_gem_object *drm_crtc_scanout(struct drm_device *dev, int crtc)
{
	if (crtc < 0 || (uint32_t)crtc >= dev->ncrtc)
		return NULL;
	struct drm_framebuffer *fb = drm_fb_lookup(dev, dev->crtc[crtc].fb_id);
	return fb ? fb->obj : NULL;
}

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

	mm_memset(c, 0, sizeof(*c));
	mm_memset(e, 0, sizeof(*e));
	mm_memset(cr, 0, sizeof(*cr));
	cr->id = mode_id_alloc(dev);
	cr->index = i;
	cr->primary_plane_id = mode_id_alloc(dev);
	cr->cursor_plane_id = mode_id_alloc(dev);
	for (int g = 0; g < 256; g++) {
		cr->gamma[0][g] = (uint16_t)(g << 8);
		cr->gamma[1][g] = (uint16_t)(g << 8);
		cr->gamma[2][g] = (uint16_t)(g << 8);
	}
	e->id = mode_id_alloc(dev);
	e->type = DRM_MODE_ENCODER_VIRTUAL;
	e->possible_crtcs = 1u << i;
	c->id = mode_id_alloc(dev);
	c->encoder_id = e->id;
	c->type = type;
	c->type_id = (uint32_t)i + 1;
	c->connected = 1;
	c->mm_width = mm_w;
	c->mm_height = mm_h;
	c->dpms = 0;
	dev->vbl[i].period_ns = 1000000000ULL / (dev->refresh_hz ? dev->refresh_hz : 60);
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

/* ---- vblank ---------------------------------------------------------- */

static void vbl_deliver(struct drm_device *dev, int crtc);

/* The vblank lifecycle: one rule, one lock, and no cancellation.
 *
 * Everything that decides whether the timer should run -- `running', `refs',
 * the CRTC's active flag as consulted here -- is read and written under
 * g_vbl_lock.  And nothing ever calls hrtimer_cancel: a timer that is no
 * longer needed is not stopped, it DECLINES TO RE-ARM at its next fire.
 *
 * Both points exist because the previous shape -- unlocked state, synchronous
 * hrtimer_cancel in vbl_stop -- had a race that wedged the counter for good:
 * one processor deciding to stop (running = 0, about to cancel) while another
 * decided to start (sees running == 0, sets it, arms the timer) ends with the
 * first processor CANCELLING THE FRESH TIMER and `running' left at 1.  From
 * then on every start returns immediately ("already running"), the timer is
 * never armed again, the counter never advances, and every waiter loops
 * timeout -> retry for the life of the machine -- seen as a browser thread
 * parked on vbl_wq in every hang dump, and a page load frozen part-way.  The
 * refcounting that made waits safe also made this path hot enough to hit the
 * race within a page load.
 *
 * With termination owned solely by the callback there is nothing to cancel
 * and no ordering to lose: the only writer that arms is whoever moved
 * `running' from 0 to 1 (under the lock), and the only re-armer is the
 * callback itself while `running' stays 1.  The reference reaches the same
 * safety by putting vblank state under a spinlock and DEFERRING disable to a
 * timer instead of doing it synchronously in hot paths; the cost here is the
 * same as there -- the counter runs at most one period past its last use. */
static spinlock_t g_vbl_lock = SPINLOCK_INIT("drm_vbl");

static void vbl_timer_fire(hrtimer_t *t)
{
	struct drm_device *dev = t->arg;
	int i = (int)(t - &dev->vbl[0].timer) / (int)((char *)&dev->vbl[1] - (char *)&dev->vbl[0]);

	if (i < 0 || (uint32_t)i >= dev->ncrtc)
		return;
	vbl_deliver(dev, i);
	{
		uint64_t fl;
		int keep;

		spin_lock_irqsave(&g_vbl_lock, &fl);
		keep = dev->crtc[i].active || dev->vbl[i].refs > 0;
		if (!keep)
			dev->vbl[i].running = 0;
		spin_unlock_irqrestore(&g_vbl_lock, fl);
		if (keep)
			hrtimer_start(&dev->vbl[i].timer,
				      dev->vbl[i].last_ns + dev->vbl[i].period_ns);
	}
}

/* Make the counter run if anything needs it.  Idempotent, callable from any
 * process context; the timer itself was initialised once at drm_kms_init and
 * is never re-initialised (re-initialising a queued hrtimer corrupts the
 * timer list, which is why vbl_start doing it on every start had to go). */
static void vbl_sync(struct drm_device *dev, int i)
{
	uint64_t fl;
	int arm = 0;

	if (dev->drv->hw_vblank)
		return;
	spin_lock_irqsave(&g_vbl_lock, &fl);
	if (!dev->vbl[i].running &&
	    (dev->crtc[i].active || dev->vbl[i].refs > 0)) {
		dev->vbl[i].running = 1;
		dev->vbl[i].last_ns = hrtimer_now_ns();
		arm = 1;
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	if (arm)
		hrtimer_start(&dev->vbl[i].timer,
			      dev->vbl[i].last_ns + dev->vbl[i].period_ns);
}

static void vbl_get(struct drm_device *dev, int i)
{
	uint64_t fl;

	spin_lock_irqsave(&g_vbl_lock, &fl);
	dev->vbl[i].refs++;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	vbl_sync(dev, i);
}

static void vbl_put(struct drm_device *dev, int i)
{
	uint64_t fl;

	spin_lock_irqsave(&g_vbl_lock, &fl);
	if (dev->vbl[i].refs > 0)
		dev->vbl[i].refs--;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	/* No stop: if this was the last need, the callback notices at its
	 * next fire and lets the timer die there. */
}

/* Pending vblank-event requests: kept on the device, delivered to the
 * requesting file when the counter reaches the target. */
struct vbl_waiter {
	struct vbl_waiter *next;
	struct drm_file *fp;
	int crtc;
	uint64_t target;
	uint64_t user_data;
	int is_seq; /* drm_event_crtc_sequence rather than vblank */
	int is_flip; /* DRM_EVENT_FLIP_COMPLETE */
};
static struct vbl_waiter *g_vbl_waiters;
static void vbl_deliver(struct drm_device *dev, int crtc)
{
	uint64_t fl;
	uint64_t now = hrtimer_now_ns();

	spin_lock_irqsave(&g_vbl_lock, &fl);
	dev->vbl[crtc].count++;
	dev->vbl[crtc].last_ns = now;
	uint64_t count = dev->vbl[crtc].count;
	struct vbl_waiter **pp = &g_vbl_waiters, *due = NULL;
	while (*pp) {
		struct vbl_waiter *w = *pp;
		if (w->fp->dev == dev && w->crtc == crtc &&
		    (int64_t)(count - w->target) >= 0) {
			*pp = w->next;
			w->next = due;
			due = w;
			continue;
		}
		pp = &w->next;
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);

	while (due) {
		struct vbl_waiter *w = due;
		due = w->next;
		if (w->is_seq) {
			struct drm_event_crtc_sequence ev;
			mm_memset(&ev, 0, sizeof(ev));
			ev.base.type = DRM_EVENT_CRTC_SEQUENCE;
			ev.base.length = sizeof(ev);
			ev.user_data = w->user_data;
			ev.time_ns = (int64_t)now;
			ev.sequence = count;
			drm_event_queue(w->fp, &ev, sizeof(ev));
		} else {
			struct drm_event_vblank ev;
			mm_memset(&ev, 0, sizeof(ev));
			ev.base.type = w->is_flip ? DRM_EVENT_FLIP_COMPLETE :
						    DRM_EVENT_VBLANK;
			ev.base.length = sizeof(ev);
			ev.user_data = w->user_data;
			ev.tv_sec = (uint32_t)(now / 1000000000ULL);
			ev.tv_usec = (uint32_t)((now % 1000000000ULL) / 1000);
			ev.sequence = (uint32_t)count;
			ev.crtc_id = dev->crtc[crtc].id;
			drm_event_queue(w->fp, &ev, sizeof(ev));
		}
		kfree(w);
	}
	poll_notify_wq(&dev->vbl_wq);
}

void drm_vblank_tick(struct drm_device *dev, int crtc)
{
	if (crtc >= 0 && (uint32_t)crtc < dev->ncrtc)
		vbl_deliver(dev, crtc);
}

static int vbl_queue_event(struct drm_device *dev, struct drm_file *fp, int crtc,
			   uint64_t target, uint64_t user_data, int is_seq,
			   int is_flip)
{
	struct vbl_waiter *w = kalloc(sizeof(*w));
	uint64_t fl;

	if (!w)
		return -ENOMEM;
	w->fp = fp;
	w->crtc = crtc;
	w->target = target;
	w->user_data = user_data;
	w->is_seq = is_seq;
	w->is_flip = is_flip;
	spin_lock_irqsave(&g_vbl_lock, &fl);
	w->next = g_vbl_waiters;
	g_vbl_waiters = w;
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	vbl_sync(dev, crtc);
	return 0;
}

void drm_kms_file_release(struct drm_device *dev, struct drm_file *fp)
{
	uint64_t fl;

	/* Events this file was waiting for. */
	spin_lock_irqsave(&g_vbl_lock, &fl);
	struct vbl_waiter **pp = &g_vbl_waiters;
	while (*pp) {
		struct vbl_waiter *w = *pp;
		if (w->fp == fp) {
			*pp = w->next;
			kfree(w);
			continue;
		}
		pp = &w->next;
	}
	spin_unlock_irqrestore(&g_vbl_lock, fl);
	/* Framebuffers it created (a master's are dropped with it). */
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		if (dev->fbs[i].id && dev->fbs[i].owner == fp) {
			for (uint32_t c = 0; c < dev->ncrtc; c++)
				if (dev->crtc[c].fb_id == dev->fbs[i].id) {
					dev->crtc[c].fb_id = 0;
					if (dev->drv->crtc_disable)
						dev->drv->crtc_disable(dev, &dev->crtc[c]);
					dev->crtc[c].active = 0;
					vbl_sync(dev, (int)c);
				}
			drm_gem_put(dev->fbs[i].obj);
			dev->fbs[i].id = 0;
		}
	}
}

/* Block until the counter passes `target' (WAIT_VBLANK without EVENT). */
static int vbl_wait_sync(struct drm_device *dev, int crtc, uint64_t target)
{
	task_t *cur = sched_current();
	/* The reference caps this wait rather than trusting the counter to
	 * arrive; a target that can never be reached must not park a thread
	 * for the life of the process. */
	uint64_t deadline = timer_ticks() + timer_ms_to_ticks(3000) + 1;
	int rc = 0;

	vbl_get(dev, crtc);
	while ((int64_t)(dev->vbl[crtc].count - target) < 0) {
		if (signal_pending(cur)) {
			rc = -EINTR;
			break;
		}
		if ((int64_t)(timer_ticks() - deadline) >= 0) {
			rc = -EBUSY;
			break;
		}
		struct wait_queue_entry we;
		uint64_t fl = local_irq_save();
		wq_entry_init(&we, cur);
		wq_add(&dev->vbl_wq, &we);
		if ((int64_t)(dev->vbl[crtc].count - target) < 0) {
			cur->wait_channel = &dev->vbl_wq;
			cur->wakeup_tick = timer_ticks() + timer_ms_to_ticks(50) + 1;
			cur->state = TASK_BLOCKED;
			local_irq_restore(fl);
			sched_schedule();
			/* Disarm.  A deadline outlives the wait that set it:
			 * sched_wake_expired_sleepers() matches any BLOCKED
			 * task whose wakeup_tick is non-zero and in the past,
			 * with no channel to qualify it, so one left behind
			 * here is claimed on the next tick of whatever this
			 * task parks on NEXT -- a pipe read, an rwsem, a futex,
			 * none of which arm a deadline of their own and so none
			 * of which overwrite it.  The task then spins at the
			 * tick rate on a wait that should have been quiet.
			 * Whoever woke us has usually cleared it already; doing
			 * it here covers the timeout case, which has not. */
			cur->wakeup_tick = 0;
			cur->wait_channel = NULL;
		} else {
			local_irq_restore(fl);
		}
		wq_remove(&dev->vbl_wq, &we);
	}
	vbl_put(dev, crtc);
	return rc;
}

/* ---- init ----------------------------------------------------------- */

void drm_kms_init(struct drm_device *dev)
{
	struct drm_prop *p;

	if (!dev->refresh_hz)
		dev->refresh_hz = 60;
	/* Once, here, and never again: the arming paths must not re-initialise
	 * a timer that may be queued.  See the vblank lifecycle comment. */
	for (int i = 0; i < DRM_MAX_CONNECTORS; i++)
		hrtimer_init(&dev->vbl[i].timer, vbl_timer_fire, dev);
	p = prop_add(dev, "DPMS", DRM_MODE_PROP_ENUM);
	prop_enum(p, 0, "On");
	prop_enum(p, 1, "Standby");
	prop_enum(p, 2, "Suspend");
	prop_enum(p, 3, "Off");
	dev->prop_dpms = p->id;
	p = prop_add(dev, "EDID", DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE);
	dev->prop_edid = p->id;
	p = prop_add(dev, "CRTC_ID", DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC);
	p->values[0] = DRM_MODE_OBJECT_CRTC;
	dev->prop_crtc_id = p->id;
	p = prop_add(dev, "link-status", DRM_MODE_PROP_ENUM);
	prop_enum(p, 0, "Good");
	prop_enum(p, 1, "Bad");
	dev->prop_link_status = p->id;
	p = prop_add(dev, "non-desktop", DRM_MODE_PROP_RANGE | DRM_MODE_PROP_IMMUTABLE);
	p->values[1] = 1;
	dev->prop_non_desktop = p->id;
	p = prop_add(dev, "type", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE);
	prop_enum(p, DRM_PLANE_TYPE_OVERLAY, "Overlay");
	prop_enum(p, DRM_PLANE_TYPE_PRIMARY, "Primary");
	prop_enum(p, DRM_PLANE_TYPE_CURSOR, "Cursor");
	dev->prop_type = p->id;
	p = prop_add(dev, "FB_ID", DRM_MODE_PROP_OBJECT | DRM_MODE_PROP_ATOMIC);
	p->values[0] = DRM_MODE_OBJECT_FB;
	dev->prop_fb_id = p->id;
	p = prop_add(dev, "ACTIVE", DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC);
	p->values[1] = 1;
	dev->prop_active = p->id;
	p = prop_add(dev, "MODE_ID", DRM_MODE_PROP_BLOB | DRM_MODE_PROP_ATOMIC);
	dev->prop_mode_id = p->id;
	static const char *const srcn[] = { "SRC_X", "SRC_Y", "SRC_W", "SRC_H" };
	uint32_t *srcp[] = { &dev->prop_src_x, &dev->prop_src_y, &dev->prop_src_w,
			     &dev->prop_src_h };
	for (int i = 0; i < 4; i++) {
		p = prop_add(dev, srcn[i], DRM_MODE_PROP_RANGE | DRM_MODE_PROP_ATOMIC);
		p->values[1] = 0xFFFFFFFFULL;
		*srcp[i] = p->id;
	}
	static const char *const crtn[] = { "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H" };
	uint32_t *crtp[] = { &dev->prop_crtc_x, &dev->prop_crtc_y, &dev->prop_crtc_w,
			     &dev->prop_crtc_h };
	for (int i = 0; i < 4; i++) {
		p = prop_add(dev, crtn[i], (i < 2 ? DRM_MODE_PROP_SIGNED_RANGE : DRM_MODE_PROP_RANGE) | DRM_MODE_PROP_ATOMIC);
		p->values[0] = i < 2 ? (uint64_t)-2147483648LL : 0;
		p->values[1] = 2147483647ULL;
		*crtp[i] = p->id;
	}
	/* No GAMMA_LUT_SIZE.  Advertising it is a claim that the CRTC takes a
	 * lookup table through the GAMMA_LUT blob property -- a display
	 * server reads the size, switches to that path, and asserts on the
	 * property it was promised.  The table arrives through the older
	 * SETGAMMA call instead, which this driver does implement, and which
	 * is what a CRTC without the property is asked with. */
	p = prop_add(dev, "IN_FORMATS", DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_ATOMIC);
	dev->prop_in_formats = p->id;

	/* IN_FORMATS blob: XR24 and AR24, LINEAR modifier only. */
	{
		uint32_t nformats = 2;
		uint32_t formats[2] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
		struct drm_format_modifier mods[1];
		uint32_t fo = sizeof(struct drm_format_modifier_blob);
		uint32_t mo = (fo + nformats * 4 + 7) & ~7u;
		uint32_t len = mo + sizeof(mods);
		uint8_t *b = kalloc(len);
		if (b) {
			struct drm_format_modifier_blob hdr;
			mm_memset(&hdr, 0, sizeof(hdr));
			hdr.version = FORMAT_BLOB_CURRENT;
			hdr.count_formats = nformats;
			hdr.formats_offset = fo;
			hdr.count_modifiers = 1;
			hdr.modifiers_offset = mo;
			mm_memset(mods, 0, sizeof(mods));
			mods[0].formats = 0x3;
			mods[0].modifier = DRM_FORMAT_MOD_LINEAR;
			mm_memcpy(b, &hdr, sizeof(hdr));
			mm_memcpy(b + fo, formats, sizeof(formats));
			mm_memcpy(b + mo, mods, sizeof(mods));
			dev->in_formats_blob = blob_create(dev, b, len);
			kfree(b);
		}
	}
	dev->min_width = 64;
	dev->min_height = 64;
	if (!dev->max_width)
		dev->max_width = 8192;
	if (!dev->max_height)
		dev->max_height = 8192;
}

/* ---- framebuffers ------------------------------------------------------ */

static int fb_create(struct drm_device *dev, struct drm_file *fp,
		     struct drm_mode_fb_cmd2 *r, uint32_t *id_out)
{
	uint32_t bpp, depth;

	switch (r->pixel_format) {
	case DRM_FORMAT_XRGB8888:
		bpp = 32;
		depth = 24;
		break;
	case DRM_FORMAT_ARGB8888:
		bpp = 32;
		depth = 32;
		break;
	case DRM_FORMAT_RGB565:
		bpp = 16;
		depth = 16;
		break;
	default:
		return -EINVAL;
	}
	if ((r->flags & DRM_MODE_FB_MODIFIERS) && r->modifier[0] != DRM_FORMAT_MOD_LINEAR)
		return -EINVAL;
	if (r->width < dev->min_width || r->height < dev->min_height ||
	    r->width > dev->max_width || r->height > dev->max_height)
		return -EINVAL;
	struct drm_gem_object *o = drm_gem_lookup(fp, r->handles[0]);
	if (!o)
		return -ENOENT;
	if (r->pitches[0] < r->width * (bpp / 8) ||
	    (uint64_t)r->offsets[0] + (uint64_t)r->pitches[0] * r->height > o->size) {
		drm_gem_put(o);
		return -EINVAL;
	}
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		if (dev->fbs[i].id)
			continue;
		struct drm_framebuffer *fb = &dev->fbs[i];
		fb->id = mode_id_alloc(dev);
		fb->width = r->width;
		fb->height = r->height;
		fb->pitch = r->pitches[0];
		fb->offset = r->offsets[0];
		fb->format = r->pixel_format;
		fb->bpp = bpp;
		fb->depth = depth;
		fb->modifier = DRM_FORMAT_MOD_LINEAR;
		fb->obj = o; /* keeps the lookup reference */
		fb->owner = fp;
		*id_out = fb->id;
		return 0;
	}
	drm_gem_put(o);
	return -ENOSPC;
}

static int crtc_set(struct drm_device *dev, struct drm_crtc *crtc,
		    const struct drm_mode_modeinfo *mode, uint32_t fb_id, int x,
		    int y);

/* ---- framebuffers and mode sets owned by the KERNEL ------------------- */
/*
 * The in-kernel console client (drm_console.c) needs both, and neither can
 * go through the ioctl paths: it has no drm_file to own a handle, its
 * framebuffer has to outlive every client that comes and goes, and it must
 * not show up in any client's resource list -- GETRESOURCES filters
 * framebuffers on the owning file, and a NULL owner matches none of them.
 */
int drm_kms_fb_add_kernel(struct drm_device *dev, struct drm_gem_object *o,
			  uint32_t w, uint32_t h, uint32_t pitch,
			  uint32_t *id_out)
{
	for (int i = 0; i < DRM_MAX_FBS; i++) {
		if (dev->fbs[i].id)
			continue;
		struct drm_framebuffer *fb = &dev->fbs[i];
		mm_memset(fb, 0, sizeof(*fb));
		fb->id = mode_id_alloc(dev);
		fb->width = w;
		fb->height = h;
		fb->pitch = pitch;
		fb->format = DRM_FORMAT_XRGB8888;
		fb->bpp = 32;
		fb->depth = 24;
		fb->modifier = DRM_FORMAT_MOD_LINEAR;
		fb->obj = o;
		fb->owner = NULL;
		drm_gem_get(o);
		*id_out = fb->id;
		return 0;
	}
	return -ENOSPC;
}

int drm_kms_crtc_set_kernel(struct drm_device *dev, uint32_t crtc_index,
			    const struct drm_mode_modeinfo *mode, uint32_t fb_id)
{
	if (crtc_index >= dev->ncrtc)
		return -ENODEV;
	return crtc_set(dev, &dev->crtc[crtc_index], mode, fb_id, 0, 0);
}

static int fb_remove(struct drm_device *dev, uint32_t id)
{
	struct drm_framebuffer *fb = drm_fb_lookup(dev, id);
	if (!fb)
		return -ENOENT;
	for (uint32_t c = 0; c < dev->ncrtc; c++) {
		if (dev->crtc[c].fb_id == id) {
			dev->crtc[c].fb_id = 0;
			if (dev->drv->crtc_disable)
				dev->drv->crtc_disable(dev, &dev->crtc[c]);
			dev->crtc[c].active = 0;
			vbl_sync(dev, (int)c);
		}
	}
	drm_gem_put(fb->obj);
	fb->obj = NULL;
	fb->id = 0;
	return 0;
}

static int crtc_set(struct drm_device *dev, struct drm_crtc *crtc,
		    const struct drm_mode_modeinfo *mode, uint32_t fb_id, int x,
		    int y)
{
	if (!fb_id || !mode) {
		crtc->fb_id = 0;
		crtc->active = 0;
		if (dev->drv->crtc_disable)
			dev->drv->crtc_disable(dev, crtc);
		vbl_sync(dev, crtc->index);
		return 0;
	}
	struct drm_framebuffer *fb = drm_fb_lookup(dev, fb_id);
	if (!fb)
		return -ENOENT;
	if (mode->hdisplay + x > fb->width || mode->vdisplay + y > fb->height)
		return -ENOSPC;
	int rc = dev->drv->mode_set ? dev->drv->mode_set(dev, crtc, mode, fb, x, y) : -ENODEV;
	if (rc)
		return rc;
	crtc->mode = *mode;
	crtc->fb_id = fb_id;
	crtc->x = x;
	crtc->y = y;
	crtc->active = 1;
	dev->conn[crtc->index].crtc_id = crtc->id;
	dev->enc[crtc->index].crtc_id = crtc->id;
	vbl_sync(dev, crtc->index);
	return 0;
}

/* ---- ioctls ---------------------------------------------------------- */

static int copy_ids(uint64_t uptr, uint32_t cap, const uint32_t *ids, uint32_t n)
{
	if (!uptr || !cap)
		return 0;
	uint32_t c = n < cap ? n : cap;
	if (!validate_user_ptr(uptr, c * 4))
		return -EFAULT;
	return copy_to_user((void *)(uintptr_t)uptr, ids, c * 4) == 0 ? 0 : -EFAULT;
}

long drm_kms_ioctl(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		   void *kb, unsigned size, int *handled)
{
	(void)size;
	*handled = 1;

	/* Everything below needs mode-setting rights except the pure
	 * queries, which any file on the primary node may use. */
	int query = (nr == 0xA0 || nr == 0xA1 || nr == 0xA6 || nr == 0xA7 ||
		     nr == 0xAA || nr == 0xAC || nr == 0xB5 || nr == 0xB9 ||
		     nr == 0xB6 || nr == 0x3a || nr == 0x3b || nr == 0xB2 ||
		     nr == 0xB3 || nr == 0xB4 || nr == 0xCE);
	if (!query && !fp->is_master && nr != 0x3c)
		return -EACCES;

	switch (nr) {
	case 0xA0: { /* GETRESOURCES */
		struct drm_mode_card_res *r = kb;
		uint32_t ids[DRM_MAX_CONNECTORS];
		uint32_t fbids[DRM_MAX_FBS];
		uint32_t nfb = 0;
		int rc;
		for (int i = 0; i < DRM_MAX_FBS; i++)
			if (dev->fbs[i].id && dev->fbs[i].owner == fp)
				fbids[nfb++] = dev->fbs[i].id;
		for (uint32_t i = 0; i < dev->ncrtc; i++)
			ids[i] = dev->crtc[i].id;
		if ((rc = copy_ids(r->crtc_id_ptr, r->count_crtcs, ids, dev->ncrtc)))
			return rc;
		for (uint32_t i = 0; i < dev->nconn; i++)
			ids[i] = dev->conn[i].id;
		if ((rc = copy_ids(r->connector_id_ptr, r->count_connectors, ids, dev->nconn)))
			return rc;
		for (uint32_t i = 0; i < dev->nenc; i++)
			ids[i] = dev->enc[i].id;
		if ((rc = copy_ids(r->encoder_id_ptr, r->count_encoders, ids, dev->nenc)))
			return rc;
		if ((rc = copy_ids(r->fb_id_ptr, r->count_fbs, fbids, nfb)))
			return rc;
		r->count_crtcs = dev->ncrtc;
		r->count_connectors = dev->nconn;
		r->count_encoders = dev->nenc;
		r->count_fbs = nfb;
		r->min_width = dev->min_width;
		r->min_height = dev->min_height;
		r->max_width = dev->max_width;
		r->max_height = dev->max_height;
		return 0;
	}
	case 0xA1: { /* GETCRTC */
		struct drm_mode_crtc *c = kb;
		struct drm_crtc *cr = crtc_find(dev, c->crtc_id);
		if (!cr)
			return -ENOENT;
		c->fb_id = cr->fb_id;
		c->x = (uint32_t)cr->x;
		c->y = (uint32_t)cr->y;
		c->gamma_size = 256;
		c->mode_valid = cr->active;
		c->mode = cr->mode;
		c->count_connectors = 0;
		return 0;
	}
	case 0xA2: { /* SETCRTC */
		struct drm_mode_crtc *c = kb;
		struct drm_crtc *cr = crtc_find(dev, c->crtc_id);
		if (!cr)
			return -ENOENT;
		if (c->count_connectors > 0 && c->set_connectors_ptr) {
			uint32_t cid;
			if (copy_from_user(&cid, (void *)(uintptr_t)c->set_connectors_ptr, 4) != 0)
				return -EFAULT;
			if (!conn_find(dev, cid))
				return -ENOENT;
		}
		if (!c->mode_valid || !c->fb_id)
			return crtc_set(dev, cr, NULL, 0, 0, 0);
		return crtc_set(dev, cr, &c->mode, c->fb_id, (int)c->x, (int)c->y);
	}
	case 0xA3: /* CURSOR */
	case 0xBB: { /* CURSOR2 */
		struct drm_mode_cursor2 *c = kb;
		struct drm_crtc *cr = crtc_find(dev, c->crtc_id);
		if (!cr)
			return -ENOENT;
		if (c->flags & DRM_MODE_CURSOR_BO) {
			struct drm_gem_object *o = NULL;
			if (c->handle) {
				o = drm_gem_lookup(fp, c->handle);
				if (!o)
					return -ENOENT;
			}
			int32_t hx = nr == 0xBB ? c->hot_x : 0;
			int32_t hy = nr == 0xBB ? c->hot_y : 0;
			int rc = dev->drv->cursor_set ?
					 dev->drv->cursor_set(dev, cr, o, c->width, c->height, hx, hy) :
					 -ENODEV;
			if (cr->cursor_obj)
				drm_gem_put(cr->cursor_obj);
			cr->cursor_obj = o; /* keeps the reference */
			cr->cursor_handle_w = c->width;
			cr->cursor_handle_h = c->height;
			if (rc)
				return rc;
		}
		if (c->flags & DRM_MODE_CURSOR_MOVE) {
			cr->cursor_x = c->x;
			cr->cursor_y = c->y;
			if (dev->drv->cursor_move)
				return dev->drv->cursor_move(dev, cr, c->x, c->y);
		}
		return 0;
	}
	case 0xA4: { /* GETGAMMA */
		struct drm_mode_crtc_lut *l = kb;
		struct drm_crtc *cr = crtc_find(dev, l->crtc_id);
		if (!cr || l->gamma_size != 256)
			return -EINVAL;
		if (copy_to_user((void *)(uintptr_t)l->red, cr->gamma[0], 512) != 0 ||
		    copy_to_user((void *)(uintptr_t)l->green, cr->gamma[1], 512) != 0 ||
		    copy_to_user((void *)(uintptr_t)l->blue, cr->gamma[2], 512) != 0)
			return -EFAULT;
		return 0;
	}
	case 0xA5: { /* SETGAMMA */
		struct drm_mode_crtc_lut *l = kb;
		struct drm_crtc *cr = crtc_find(dev, l->crtc_id);
		if (!cr || l->gamma_size != 256)
			return -EINVAL;
		if (copy_from_user(cr->gamma[0], (void *)(uintptr_t)l->red, 512) != 0 ||
		    copy_from_user(cr->gamma[1], (void *)(uintptr_t)l->green, 512) != 0 ||
		    copy_from_user(cr->gamma[2], (void *)(uintptr_t)l->blue, 512) != 0)
			return -EFAULT;
		return 0; /* the virtual display has no LUT: accepted */
	}
	case 0xA6: { /* GETENCODER */
		struct drm_mode_get_encoder *e = kb;
		struct drm_encoder *en = enc_find(dev, e->encoder_id);
		if (!en)
			return -ENOENT;
		e->encoder_type = en->type;
		e->crtc_id = en->crtc_id;
		e->possible_crtcs = en->possible_crtcs;
		e->possible_clones = 0;
		return 0;
	}
	case 0xA7: { /* GETCONNECTOR */
		struct drm_mode_get_connector *g = kb;
		struct drm_connector *c = conn_find(dev, g->connector_id);
		if (!c)
			return -ENOENT;
		int rc;
		uint32_t enc = c->encoder_id;
		if ((rc = copy_ids(g->encoders_ptr, g->count_encoders, &enc, 1)))
			return rc;
		if (g->modes_ptr && g->count_modes) {
			uint32_t n = c->nmodes < g->count_modes ? c->nmodes : g->count_modes;
			if (!validate_user_ptr(g->modes_ptr, n * sizeof(struct drm_mode_modeinfo)) ||
			    copy_to_user((void *)(uintptr_t)g->modes_ptr, c->modes,
					 n * sizeof(struct drm_mode_modeinfo)) != 0)
				return -EFAULT;
		}
		uint32_t pids[4] = { dev->prop_dpms, dev->prop_link_status,
				     dev->prop_non_desktop, dev->prop_edid };
		uint64_t pvals[4] = { (uint64_t)c->dpms, 0, 0, c->edid_blob_id };
		uint32_t np = c->edid_blob_id ? 4 : 3;
		if ((rc = copy_ids(g->props_ptr, g->count_props, pids, np)))
			return rc;
		if (g->prop_values_ptr && g->count_props) {
			uint32_t n = np < g->count_props ? np : g->count_props;
			if (copy_to_user((void *)(uintptr_t)g->prop_values_ptr, pvals, n * 8) != 0)
				return -EFAULT;
		}
		g->encoder_id = c->encoder_id;
		g->connector_type = c->type;
		g->connector_type_id = c->type_id;
		g->connection = c->connected ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED;
		g->mm_width = c->mm_width;
		g->mm_height = c->mm_height;
		g->subpixel = DRM_MODE_SUBPIXEL_UNKNOWN;
		g->count_modes = c->nmodes;
		g->count_props = np;
		g->count_encoders = 1;
		return 0;
	}
	case 0xAA: { /* GETPROPERTY */
		struct drm_mode_get_property *g = kb;
		struct drm_prop *p = prop_find(dev, g->prop_id);
		if (!p)
			return -ENOENT;
		mm_memcpy(g->name, p->name, sizeof(g->name));
		g->flags = p->flags;
		if (p->flags & (DRM_MODE_PROP_RANGE | DRM_MODE_PROP_SIGNED_RANGE)) {
			if (g->values_ptr && g->count_values) {
				uint32_t n = g->count_values < 2 ? g->count_values : 2;
				if (copy_to_user((void *)(uintptr_t)g->values_ptr, p->values, n * 8) != 0)
					return -EFAULT;
			}
			g->count_values = 2;
			g->count_enum_blobs = 0;
		} else if (p->flags & DRM_MODE_PROP_ENUM) {
			if (g->values_ptr && g->count_values) {
				uint64_t v[8];
				for (uint32_t i = 0; i < p->nenums; i++)
					v[i] = p->enums[i].value;
				uint32_t n = g->count_values < p->nenums ? g->count_values : p->nenums;
				if (copy_to_user((void *)(uintptr_t)g->values_ptr, v, n * 8) != 0)
					return -EFAULT;
			}
			if (g->enum_blob_ptr && g->count_enum_blobs) {
				uint32_t n = g->count_enum_blobs < p->nenums ? g->count_enum_blobs : p->nenums;
				if (copy_to_user((void *)(uintptr_t)g->enum_blob_ptr, p->enums,
						 n * sizeof(struct drm_mode_property_enum)) != 0)
					return -EFAULT;
			}
			g->count_values = p->nenums;
			g->count_enum_blobs = p->nenums;
		} else if (p->flags & DRM_MODE_PROP_OBJECT) {
			if (g->values_ptr && g->count_values &&
			    copy_to_user((void *)(uintptr_t)g->values_ptr, p->values, 8) != 0)
				return -EFAULT;
			g->count_values = 1;
			g->count_enum_blobs = 0;
		} else {
			g->count_values = 0;
			g->count_enum_blobs = 0;
		}
		return 0;
	}
	case 0xAB: { /* SETPROPERTY (connector) */
		struct drm_mode_connector_set_property *s = kb;
		struct drm_connector *c = conn_find(dev, s->connector_id);
		if (!c)
			return -ENOENT;
		if (s->prop_id == dev->prop_dpms) {
			c->dpms = (int)s->value;
			if (dev->drv->dpms)
				dev->drv->dpms(dev, c, (int)s->value);
			return 0;
		}
		return -EINVAL;
	}
	case 0xAC: { /* GETPROPBLOB */
		struct drm_mode_get_blob *g = kb;
		struct drm_blob *b = blob_find(dev, g->blob_id);
		if (!b)
			return -ENOENT;
		if (g->data && g->length) {
			uint32_t n = g->length < b->length ? g->length : b->length;
			if (!validate_user_ptr(g->data, n) ||
			    copy_to_user((void *)(uintptr_t)g->data, b->data, n) != 0)
				return -EFAULT;
		}
		g->length = b->length;
		return 0;
	}
	case 0xAE: { /* ADDFB */
		struct drm_mode_fb_cmd *f = kb;
		struct drm_mode_fb_cmd2 r;
		mm_memset(&r, 0, sizeof(r));
		r.width = f->width;
		r.height = f->height;
		r.pitches[0] = f->pitch;
		r.handles[0] = f->handle;
		if (f->bpp == 32 && f->depth == 24)
			r.pixel_format = DRM_FORMAT_XRGB8888;
		else if (f->bpp == 32 && f->depth == 32)
			r.pixel_format = DRM_FORMAT_ARGB8888;
		else if (f->bpp == 16)
			r.pixel_format = DRM_FORMAT_RGB565;
		else
			return -EINVAL;
		return fb_create(dev, fp, &r, &f->fb_id);
	}
	case 0xB8: { /* ADDFB2 */
		struct drm_mode_fb_cmd2 *r = kb;
		if (r->handles[1] || r->handles[2] || r->handles[3])
			return -EINVAL;
		return fb_create(dev, fp, r, &r->fb_id);
	}
	case 0xAF: { /* RMFB */
		uint32_t id = *(uint32_t *)kb;
		return fb_remove(dev, id);
	}
	case 0xAD: { /* GETFB */
		struct drm_mode_fb_cmd *f = kb;
		struct drm_framebuffer *fb = drm_fb_lookup(dev, f->fb_id);
		if (!fb)
			return -ENOENT;
		f->width = fb->width;
		f->height = fb->height;
		f->pitch = fb->pitch;
		f->bpp = fb->bpp;
		f->depth = fb->depth;
		uint32_t h;
		if (drm_gem_handle_create(fp, fb->obj, &h) != 0)
			return -ENOMEM;
		f->handle = h;
		return 0;
	}
	case 0xCE: { /* GETFB2 */
		struct drm_mode_fb_cmd2 *r = kb;
		struct drm_framebuffer *fb = drm_fb_lookup(dev, r->fb_id);
		if (!fb)
			return -ENOENT;
		mm_memset(r->handles, 0, sizeof(r->handles));
		mm_memset(r->pitches, 0, sizeof(r->pitches));
		mm_memset(r->offsets, 0, sizeof(r->offsets));
		r->width = fb->width;
		r->height = fb->height;
		r->pixel_format = fb->format;
		r->pitches[0] = fb->pitch;
		r->offsets[0] = fb->offset;
		r->flags = DRM_MODE_FB_MODIFIERS;
		r->modifier[0] = fb->modifier;
		uint32_t h;
		if (drm_gem_handle_create(fp, fb->obj, &h) != 0)
			return -ENOMEM;
		r->handles[0] = h;
		return 0;
	}
	case 0xB0: { /* PAGE_FLIP */
		struct drm_mode_crtc_page_flip_target *f = kb;
		struct drm_crtc *cr = crtc_find(dev, f->crtc_id);
		if (!cr)
			return -ENOENT;
		if (f->flags & ~(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC))
			return -EINVAL;
		struct drm_framebuffer *fb = drm_fb_lookup(dev, f->fb_id);
		if (!fb)
			return -ENOENT;
		if (!cr->active)
			return -EINVAL;
		int rc = dev->drv->page_flip ? dev->drv->page_flip(dev, cr, fb) : -EINVAL;
		if (rc)
			return rc;
		cr->fb_id = fb->id;
		if (f->flags & DRM_MODE_PAGE_FLIP_EVENT)
			return vbl_queue_event(dev, fp, cr->index,
					       dev->vbl[cr->index].count + 1,
					       f->user_data, 0, 1);
		return 0;
	}
	case 0xB1: { /* DIRTYFB */
		struct drm_mode_fb_dirty_cmd *d = kb;
		struct drm_framebuffer *fb = drm_fb_lookup(dev, d->fb_id);
		if (!fb)
			return -ENOENT;
		struct drm_mode_rect_k rects[64];
		uint32_t n = d->num_clips;
		if (n > 256)
			return -EINVAL;
		int rc = 0;
		for (uint32_t c = 0; c < dev->ncrtc; c++) {
			struct drm_crtc *cr = &dev->crtc[c];
			if (cr->fb_id != fb->id || !dev->drv->fb_dirty)
				continue;
			if (n == 0) {
				struct drm_mode_rect_k all = { 0, 0, (int32_t)fb->width, (int32_t)fb->height };
				rc = dev->drv->fb_dirty(dev, cr, fb, &all, 1);
				continue;
			}
			for (uint32_t done = 0; done < n; done += 64) {
				uint32_t k = n - done < 64 ? n - done : 64;
				struct drm_clip_rect cl[64];
				if (copy_from_user(cl, (void *)(uintptr_t)(d->clips_ptr + done * sizeof(struct drm_clip_rect)),
						   k * sizeof(struct drm_clip_rect)) != 0)
					return -EFAULT;
				for (uint32_t i = 0; i < k; i++) {
					rects[i].x1 = cl[i].x1;
					rects[i].y1 = cl[i].y1;
					rects[i].x2 = cl[i].x2;
					rects[i].y2 = cl[i].y2;
				}
				rc = dev->drv->fb_dirty(dev, cr, fb, rects, k);
				if (rc)
					break;
			}
		}
		return rc;
	}
	case 0xB2: { /* CREATE_DUMB */
		struct drm_mode_create_dumb *c = kb;
		if (c->bpp != 32 && c->bpp != 16)
			return -EINVAL;
		if (c->width == 0 || c->height == 0 || c->width > 16384 || c->height > 16384)
			return -EINVAL;
		uint32_t pitch = (c->width * (c->bpp / 8) + 63) & ~63u;
		uint64_t size = (uint64_t)pitch * c->height;
		struct drm_gem_object *o = drm_gem_alloc(dev, DRM_GEM_BO, size);
		if (!o)
			return -ENOMEM;
		o->scanout = 1;
		o->width = c->width;
		o->height = c->height;
		o->pitch = pitch;
		o->format = c->bpp == 32 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_RGB565;
		int rc = drm_gem_alloc_pages(o);
		if (rc == 0 && dev->drv->gem_init)
			rc = dev->drv->gem_init(o);
		if (rc) {
			drm_gem_put(o);
			return rc;
		}
		uint32_t h;
		rc = drm_gem_handle_create(fp, o, &h);
		drm_gem_put(o);
		if (rc)
			return rc;
		c->handle = h;
		c->pitch = pitch;
		c->size = size;
		return 0;
	}
	case 0xB3: { /* MAP_DUMB */
		struct drm_mode_map_dumb *m = kb;
		struct drm_gem_object *o = drm_gem_lookup(fp, m->handle);
		if (!o)
			return -ENOENT;
		m->offset = drm_gem_mmap_offset(o);
		drm_gem_put(o);
		return 0;
	}
	case 0xB4: { /* DESTROY_DUMB */
		struct drm_mode_destroy_dumb *d = kb;
		return drm_gem_handle_delete(fp, d->handle);
	}
	case 0xB5: { /* GETPLANERESOURCES */
		struct drm_mode_get_plane_res *r = kb;
		uint32_t ids[2 * DRM_MAX_CONNECTORS];
		uint32_t n = 0;
		int universal = (fp->client_caps >> DRM_CLIENT_CAP_UNIVERSAL_PLANES) & 1;
		for (uint32_t i = 0; i < dev->ncrtc; i++) {
			if (universal) {
				ids[n++] = dev->crtc[i].primary_plane_id;
				ids[n++] = dev->crtc[i].cursor_plane_id;
			}
		}
		int rc = copy_ids(r->plane_id_ptr, r->count_planes, ids, n);
		if (rc)
			return rc;
		r->count_planes = n;
		return 0;
	}
	case 0xB6: { /* GETPLANE */
		struct drm_mode_get_plane *g = kb;
		int cursor;
		struct drm_crtc *cr = plane_crtc(dev, g->plane_id, &cursor);
		if (!cr)
			return -ENOENT;
		uint32_t fmts[2] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
		int rc = copy_ids(g->format_type_ptr, g->count_format_types, fmts, 2);
		if (rc)
			return rc;
		g->count_format_types = 2;
		g->crtc_id = cr->active ? cr->id : 0;
		g->fb_id = cursor ? 0 : cr->fb_id;
		g->possible_crtcs = 1u << cr->index;
		g->gamma_size = 0;
		return 0;
	}
	case 0xB7: { /* SETPLANE */
		struct drm_mode_set_plane *s = kb;
		int cursor;
		struct drm_crtc *cr = plane_crtc(dev, s->plane_id, &cursor);
		if (!cr)
			return -ENOENT;
		if (cursor)
			return -EINVAL;
		if (!s->fb_id)
			return crtc_set(dev, cr, NULL, 0, 0, 0);
		struct drm_framebuffer *fb = drm_fb_lookup(dev, s->fb_id);
		if (!fb)
			return -ENOENT;
		if (cr->active && cr->fb_id != fb->id && dev->drv->page_flip) {
			int rc = dev->drv->page_flip(dev, cr, fb);
			if (rc)
				return rc;
			cr->fb_id = fb->id;
			return 0;
		}
		return crtc_set(dev, cr, &cr->mode, fb->id, (int)(s->src_x >> 16), (int)(s->src_y >> 16));
	}
	case 0xB9: { /* OBJ_GETPROPERTIES */
		struct drm_mode_obj_get_properties *g = kb;
		uint32_t pids[16];
		uint64_t vals[16];
		uint32_t n = 0;
		switch (g->obj_type) {
		case DRM_MODE_OBJECT_CONNECTOR: {
			struct drm_connector *c = conn_find(dev, g->obj_id);
			if (!c)
				return -ENOENT;
			pids[n] = dev->prop_dpms; vals[n++] = (uint64_t)c->dpms;
			pids[n] = dev->prop_crtc_id; vals[n++] = c->crtc_id;
			pids[n] = dev->prop_link_status; vals[n++] = 0;
			pids[n] = dev->prop_non_desktop; vals[n++] = 0;
			if (c->edid_blob_id) {
				pids[n] = dev->prop_edid; vals[n++] = c->edid_blob_id;
			}
			break;
		}
		case DRM_MODE_OBJECT_CRTC: {
			struct drm_crtc *cr = crtc_find(dev, g->obj_id);
			if (!cr)
				return -ENOENT;
			pids[n] = dev->prop_active; vals[n++] = cr->active;
			pids[n] = dev->prop_mode_id; vals[n++] = 0;
			break;
		}
		case DRM_MODE_OBJECT_PLANE: {
			int cursor;
			struct drm_crtc *cr = plane_crtc(dev, g->obj_id, &cursor);
			if (!cr)
				return -ENOENT;
			pids[n] = dev->prop_type; vals[n++] = cursor ? DRM_PLANE_TYPE_CURSOR : DRM_PLANE_TYPE_PRIMARY;
			pids[n] = dev->prop_fb_id; vals[n++] = cursor ? 0 : cr->fb_id;
			pids[n] = dev->prop_crtc_id; vals[n++] = cr->active ? cr->id : 0;
			pids[n] = dev->prop_src_x; vals[n++] = (uint64_t)cr->x << 16;
			pids[n] = dev->prop_src_y; vals[n++] = (uint64_t)cr->y << 16;
			pids[n] = dev->prop_src_w; vals[n++] = (uint64_t)cr->mode.hdisplay << 16;
			pids[n] = dev->prop_src_h; vals[n++] = (uint64_t)cr->mode.vdisplay << 16;
			pids[n] = dev->prop_crtc_x; vals[n++] = 0;
			pids[n] = dev->prop_crtc_y; vals[n++] = 0;
			pids[n] = dev->prop_crtc_w; vals[n++] = cr->mode.hdisplay;
			pids[n] = dev->prop_crtc_h; vals[n++] = cr->mode.vdisplay;
			if (!cursor) {
				pids[n] = dev->prop_in_formats; vals[n++] = dev->in_formats_blob;
			}
			break;
		}
		default:
			return -ENOENT;
		}
		int rc = copy_ids(g->props_ptr, g->count_props, pids, n);
		if (rc)
			return rc;
		if (g->prop_values_ptr && g->count_props) {
			uint32_t k = n < g->count_props ? n : g->count_props;
			if (copy_to_user((void *)(uintptr_t)g->prop_values_ptr, vals, k * 8) != 0)
				return -EFAULT;
		}
		g->count_props = n;
		return 0;
	}
	case 0xBA: { /* OBJ_SETPROPERTY */
		struct drm_mode_obj_set_property *s = kb;
		if (s->obj_type == DRM_MODE_OBJECT_CONNECTOR) {
			struct drm_connector *c = conn_find(dev, s->obj_id);
			if (!c)
				return -ENOENT;
			if (s->prop_id == dev->prop_dpms) {
				c->dpms = (int)s->value;
				if (dev->drv->dpms)
					dev->drv->dpms(dev, c, (int)s->value);
				return 0;
			}
		}
		return -EINVAL;
	}
	case 0xBD: { /* CREATEPROPBLOB */
		struct drm_mode_create_blob *c = kb;
		if (c->length == 0 || c->length > 65536)
			return -EINVAL;
		void *tmp = kalloc(c->length);
		if (!tmp)
			return -ENOMEM;
		if (!validate_user_ptr(c->data, c->length) ||
		    copy_from_user(tmp, (void *)(uintptr_t)c->data, c->length) != 0) {
			kfree(tmp);
			return -EFAULT;
		}
		c->blob_id = blob_create(dev, tmp, c->length);
		kfree(tmp);
		return c->blob_id ? 0 : -ENOSPC;
	}
	case 0xBE: { /* DESTROYPROPBLOB */
		struct drm_mode_destroy_blob *d = kb;
		if (!blob_find(dev, d->blob_id))
			return -ENOENT;
		blob_destroy(dev, d->blob_id);
		return 0;
	}
	case 0xBC: { /* ATOMIC */
		struct drm_mode_atomic *a = kb;
		if (!((fp->client_caps >> DRM_CLIENT_CAP_ATOMIC) & 1))
			return -EINVAL;
		if (a->flags & ~(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_NONBLOCK |
				 DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT))
			return -EINVAL;
		/* Gather the per-object property lists and apply what the
		 * legacy paths know: FB_ID/CRTC_ID on a plane, ACTIVE and
		 * MODE_ID on a crtc, DPMS on a connector. */
		uint32_t nobj = a->count_objs;
		if (nobj > 16)
			return -EINVAL;
		uint32_t objs[16], objcnt[16];
		if (copy_from_user(objs, (void *)(uintptr_t)a->objs_ptr, nobj * 4) != 0 ||
		    copy_from_user(objcnt, (void *)(uintptr_t)a->count_props_ptr, nobj * 4) != 0)
			return -EFAULT;
		uint32_t pidx = 0;
		struct drm_crtc *flip_crtc = NULL;
		int rc = 0;
		for (uint32_t i = 0; i < nobj && rc == 0; i++) {
			for (uint32_t j = 0; j < objcnt[i]; j++, pidx++) {
				uint32_t pid;
				uint64_t val;
				if (copy_from_user(&pid, (void *)(uintptr_t)(a->props_ptr + pidx * 4), 4) != 0 ||
				    copy_from_user(&val, (void *)(uintptr_t)(a->prop_values_ptr + pidx * 8), 8) != 0)
					return -EFAULT;
				if (a->flags & DRM_MODE_ATOMIC_TEST_ONLY)
					continue;
				int cursor;
				struct drm_crtc *cr = plane_crtc(dev, objs[i], &cursor);
				if (cr && !cursor && pid == dev->prop_fb_id) {
					if (!val) {
						rc = crtc_set(dev, cr, NULL, 0, 0, 0);
					} else {
						struct drm_framebuffer *fb = drm_fb_lookup(dev, (uint32_t)val);
						if (!fb) {
							rc = -ENOENT;
							break;
						}
						if (cr->active && dev->drv->page_flip) {
							rc = dev->drv->page_flip(dev, cr, fb);
							if (rc == 0)
								cr->fb_id = fb->id;
						} else {
							rc = crtc_set(dev, cr, &cr->mode, fb->id, 0, 0);
						}
					}
					flip_crtc = cr;
					continue;
				}
				struct drm_crtc *c2 = crtc_find(dev, objs[i]);
				if (c2 && pid == dev->prop_mode_id && val) {
					struct drm_blob *b = blob_find(dev, (uint32_t)val);
					if (!b || b->length < sizeof(struct drm_mode_modeinfo)) {
						rc = -EINVAL;
						break;
					}
					c2->mode = *(struct drm_mode_modeinfo *)b->data;
					if (c2->fb_id)
						rc = crtc_set(dev, c2, &c2->mode, c2->fb_id, c2->x, c2->y);
					continue;
				}
				if (c2 && pid == dev->prop_active) {
					if (!val)
						rc = crtc_set(dev, c2, NULL, 0, 0, 0);
					continue;
				}
				struct drm_connector *cn = conn_find(dev, objs[i]);
				if (cn && pid == dev->prop_dpms) {
					cn->dpms = (int)val;
					if (dev->drv->dpms)
						dev->drv->dpms(dev, cn, (int)val);
				}
			}
		}
		if (rc == 0 && (a->flags & DRM_MODE_PAGE_FLIP_EVENT) &&
		    !(a->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
			struct drm_crtc *cr = flip_crtc ? flip_crtc : &dev->crtc[0];
			rc = vbl_queue_event(dev, fp, cr->index, dev->vbl[cr->index].count + 1,
					     a->user_data, 0, 1);
		}
		return rc;
	}
	case 0x3a: { /* WAIT_VBLANK */
		union drm_wait_vblank *w = kb;
		uint32_t type = w->request.type;
		int crtc = 0;
		if (type & _DRM_VBLANK_HIGH_CRTC_MASK)
			crtc = (int)((type & _DRM_VBLANK_HIGH_CRTC_MASK) >> _DRM_VBLANK_HIGH_CRTC_SHIFT);
		else if (type & _DRM_VBLANK_SECONDARY)
			crtc = 1;
		if ((uint32_t)crtc >= dev->ncrtc)
			return -EINVAL;
		if (!dev->crtc[crtc].active)
			return -EINVAL;
		uint64_t cur = dev->vbl[crtc].count;
		uint64_t target;
		if ((type & _DRM_VBLANK_TYPES_MASK) == _DRM_VBLANK_RELATIVE)
			target = cur + w->request.sequence;
		else
			target = (cur & ~0xFFFFFFFFULL) | w->request.sequence;
		if ((type & _DRM_VBLANK_NEXTONMISS) && (int64_t)(target - cur) <= 0)
			target = cur + 1;
		if (type & _DRM_VBLANK_EVENT) {
			int rc = vbl_queue_event(dev, fp, crtc, target, w->request.signal, 0, 0);
			if (rc)
				return rc;
			w->reply.type = type;
			w->reply.sequence = (uint32_t)cur;
			uint64_t ns = dev->vbl[crtc].last_ns;
			w->reply.tval_sec = (long)(ns / 1000000000ULL);
			w->reply.tval_usec = (long)((ns % 1000000000ULL) / 1000);
			return 0;
		}
		int rc = vbl_wait_sync(dev, crtc, target);
		if (rc)
			return rc;
		uint64_t ns = dev->vbl[crtc].last_ns;
		w->reply.type = type;
		w->reply.sequence = (uint32_t)dev->vbl[crtc].count;
		w->reply.tval_sec = (long)(ns / 1000000000ULL);
		w->reply.tval_usec = (long)((ns % 1000000000ULL) / 1000);
		return 0;
	}
	case 0x3b: { /* CRTC_GET_SEQUENCE */
		struct drm_crtc_get_sequence *g = kb;
		struct drm_crtc *cr = crtc_find(dev, g->crtc_id);
		if (!cr)
			return -ENOENT;
		g->active = cr->active;
		g->sequence = dev->vbl[cr->index].count;
		g->sequence_ns = (int64_t)dev->vbl[cr->index].last_ns;
		return 0;
	}
	case 0x3c: { /* CRTC_QUEUE_SEQUENCE */
		struct drm_crtc_queue_sequence *q = kb;
		struct drm_crtc *cr = crtc_find(dev, q->crtc_id);
		if (!cr)
			return -ENOENT;
		if (!cr->active)
			return -EINVAL;
		uint64_t cur = dev->vbl[cr->index].count;
		uint64_t target = (q->flags & DRM_CRTC_SEQUENCE_RELATIVE) ? cur + q->sequence : q->sequence;
		if ((q->flags & DRM_CRTC_SEQUENCE_NEXT_ON_MISS) && (int64_t)(target - cur) <= 0)
			target = cur + 1;
		q->sequence = target;
		return vbl_queue_event(dev, fp, cr->index, target, q->user_data, 1, 0);
	}
	case 0xC6: /* CREATE_LEASE */
	case 0xC7: /* LIST_LESSEES */
	case 0xC8: /* GET_LEASE */
	case 0xC9: /* REVOKE_LEASE */
		return -EINVAL;
	default:
		*handled = 0;
		return -ENOTTY;
	}
}
