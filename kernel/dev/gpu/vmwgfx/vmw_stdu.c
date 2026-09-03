// LikeOS-64 -- vmwgfx: scan-out through screen targets.
//
// The device offers three ways to put a picture on the screen, and which one
// is right is decided by what the device advertises, not by preference:
//
//   legacy      the host reads VRAM; the driver copies rows into it and
//               announces the rectangle.  Works everywhere, costs a full
//               copy per update, and cannot show anything the guest did not
//               draw itself with the CPU.
//   screen objects  the host reads a GMR (a guest page list) directly, or a
//               rendered surface is blitted onto the screen with one 3D
//               command.  This is the path for devices with GMRs and no
//               guest-backed objects.
//   screen targets  what a device with guest-backed objects (MOBs, GB
//               surfaces -- everything the 3D path here uses) expects.  The
//               screen IS a surface: one GB surface is bound to the target
//               and the host scans it out, so a rendered frame reaches the
//               display without leaving host memory.
//
// This file is the third.  It matters beyond tidiness: on a device in
// guest-backed mode the older paths are not merely slower, the host may
// refuse the commands outright -- and then the desktop draws nothing while
// every 3D operation still succeeds, which is a hard failure to read from
// the outside.
//
// One target, id 0, marked primary.  Multi-head belongs here later; the
// device allows up to SVGA_REG_SCREENTARGET_MAX_WIDTH-sized targets and
// several of them, and nothing below assumes there is only one beyond the
// single id it uses.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define VMW_STDU_ID 0

/* The display surface's pixel format.  X8R8G8B8 is what the console and
 * every dumb buffer here already are, so a scan-out of one is a copy and
 * not a conversion. */
#define VMW_STDU_FORMAT SVGA3D_X8R8G8B8

static struct drm_gem_object *stdu_mob_bo(struct vmw_device *v, uint32_t size)
{
	struct drm_gem_object *bo = drm_gem_alloc(&v->drm, DRM_GEM_BO, size);

	if (!bo)
		return NULL;
	if (drm_gem_alloc_pages(bo) || v->drm.drv->gem_init(bo)) {
		drm_gem_put(bo);
		return NULL;
	}
	struct vmw_bo *b = bo->priv;
	if (!b || b->mob.id == SVGA3D_INVALID_ID) {
		drm_gem_put(bo);
		return NULL;
	}
	return bo;
}

/* Is this device supposed to scan out through a screen target?
 *
 * Both halves are required: the guest-backed object machinery has to be up
 * (the target is bound to a GB surface, which lives in a MOB), and the
 * device has to advertise a maximum screen-target size -- which is how it
 * says the feature exists at all. */
int vmw_stdu_available(struct vmw_device *v)
{
	/* On a device with guest-backed objects the screen target IS the
	 * display path -- upstream picks the display unit from that
	 * capability alone (SVGA_CAP_GBOBJECTS => vmw_du_screen_target) and
	 * reads SVGA_REG_SCREENTARGET_MAX_WIDTH only for size limits.
	 *
	 * Requiring that register to be non-zero here was a deviation with
	 * teeth: where it reads 0 the screen target was switched off, and a
	 * guest-backed surface then fell to the screen-object blit, which
	 * cannot show one -- it takes the command, answers success and
	 * displays nothing.  Every ioctl passed, no error was logged
	 * anywhere, and X drew a fully working desktop onto a black screen. */
	return v->has_gb && v->otables_ready;
}

static void stdu_drop_surface(struct vmw_device *v)
{
	/* vmw_surface_destroy() owns everything here: it sends the destroy
	 * command, drops the backup buffer's reference and frees the surface
	 * itself.  So st_bo is only forgotten, never put a second time. */
	if (v->st_surface) {
		vmw_surface_destroy(v, v->st_surface);
		v->st_surface = NULL;
	}
	v->st_bo = NULL;
}

/* Bind image `sid' to the target (SVGA3D_INVALID_ID unbinds it). */
static int stdu_bind(struct vmw_device *v, uint32_t sid)
{
	SVGA3dCmdBindGBScreenTarget c;

	mm_memset(&c, 0, sizeof(c));
	c.stid = VMW_STDU_ID;
	c.image.sid = sid;
	c.image.face = 0;
	c.image.mipmap = 0;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_BIND_GB_SCREENTARGET, &c, sizeof(c));
	if (rc == 0) {
		v->st_bound_sid = sid;
		v->st_bound_obj = NULL;   /* the caller records identity */
		vmw_execbuf_note_bind();
	}
	return rc;
}

void vmw_stdu_teardown(struct vmw_device *v)
{
	if (!v->st_defined)
		return;
	if (v->st_bound_sid != SVGA3D_INVALID_ID)
		stdu_bind(v, SVGA3D_INVALID_ID);
	SVGA3dCmdDestroyGBScreenTarget c;
	c.stid = VMW_STDU_ID;
	vmw_cmd_one(v, SVGA_3D_CMD_DESTROY_GB_SCREENTARGET, &c, sizeof(c));
	v->st_defined = 0;
	v->st_w = v->st_h = 0;
	v->st_bound_sid = SVGA3D_INVALID_ID;
	stdu_drop_surface(v);
}

/* Define the target at this size, replacing whatever was there. */
static int stdu_define(struct vmw_device *v, uint32_t w, uint32_t h)
{
	if (v->st_defined && v->st_w == w && v->st_h == h)
		return 0;
	vmw_stdu_teardown(v);

	SVGA3dCmdDefineGBScreenTarget c;
	mm_memset(&c, 0, sizeof(c));
	c.stid = VMW_STDU_ID;
	c.width = w;
	c.height = h;
	c.xRoot = 0;
	c.yRoot = 0;
	c.flags = SVGA_STFLAG_PRIMARY;
	c.dpi = 0;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SCREENTARGET, &c, sizeof(c));
	if (rc)
		return rc;
	v->st_defined = 1;
	v->st_w = w;
	v->st_h = h;
	v->st_bound_sid = SVGA3D_INVALID_ID;
	return 0;
}

/* The driver's own display surface: what is bound to the target when the
 * framebuffer cannot be bound directly (a buffer object, or a surface whose
 * geometry does not match the mode).  Its storage is a MOB the guest can
 * write, which is what makes the CPU path below possible.
 *
 * Returns 1 when a NEW surface was created, 0 when the existing one was
 * reused, negative on failure.  The distinction is not cosmetic: a surface
 * the host has just defined holds no image, so whoever created one owes the
 * screen a full refresh before a dirty rectangle means anything. */
static int stdu_display_surface_fmt(struct vmw_device *v, uint32_t w, uint32_t h,
				    uint32_t format)
{
	if (v->st_surface && v->st_surface->base_size.width == w &&
	    v->st_surface->base_size.height == h &&
	    v->st_surface->format == format)
		return 0;
	stdu_drop_surface(v);

	struct vmw_surface *s = kalloc(sizeof(*s));
	if (!s)
		return -ENOMEM;
	mm_memset(s, 0, sizeof(*s));
	int sid = vmw_surface_alloc_id(v);
	if (sid < 0) {
		kfree(s);
		return -ENOSPC;
	}
	s->sid = (uint32_t)sid;
	/* SCREENTARGET is what makes the surface scannable; the render-target
	 * hint lets the host keep it where its renderer can also draw into
	 * it, which is what a compositor does with the front buffer. */
	s->flags = SVGA3D_SURFACE_SCREENTARGET | SVGA3D_SURFACE_HINT_RENDERTARGET;
	s->format = format;
	s->mip_levels = 1;
	s->array_size = 1;
	s->base_size.width = w;
	s->base_size.height = h;
	s->base_size.depth = 1;
	s->scanout = 1;
	s->backup_size = vmw_surface_size(s->format, &s->base_size, 1, 1, 0,
					  s->flags);
	if (s->backup_size < w * h * 4)
		s->backup_size = w * h * 4;

	s->backup = stdu_mob_bo(v, s->backup_size);
	if (!s->backup) {
		vmw_surface_free_id(v, s->sid);
		kfree(s);
		return -ENOMEM;
	}
	int rc = vmw_surface_define(v, s);
	if (rc == 0)
		rc = vmw_surface_bind(v, s);
	if (rc) {
		vmw_surface_destroy(v, s); /* releases the backup and `s' */
		return rc;
	}
	v->st_surface = s;
	v->st_bo = s->backup;
	(void)format;
	return 1;
}

/* The buffer-object path writes X8R8G8B8 pixels from the guest, so that is
 * the format its display surface takes. */
static int stdu_display_surface(struct vmw_device *v, uint32_t w, uint32_t h)
{
	return stdu_display_surface_fmt(v, w, h, VMW_STDU_FORMAT);
}

/* Which path a present took, reported when it CHANGES.
 *
 * The change is the whole point: a silent switch from direct scan-out to the
 * copy path is the difference between a desktop and a black screen with the
 * last few damaged rectangles on it, and neither side of the switch fails or
 * logs anything by itself.  The budget is per event rather than one counter
 * for the file -- a shared one is spent by console flushes long before
 * startx, and the case worth reporting could then never report itself. */
static void stdu_report_path(struct vmw_device *v, const char *path,
			     struct vmw_surface *s)
{
	static const char *last;

	if (path == last)
		return;
	last = path;
	if (s)
		kprintf("[drm] vmwgfx: scan-out via %s: surface %u %ux%u fmt %u, target %ux%u\n",
			path, s->sid, s->base_size.width, s->base_size.height,
			s->format, v->st_w, v->st_h);
	else
		kprintf("[drm] vmwgfx: scan-out via %s: target %ux%u\n", path,
			v->st_w, v->st_h);
}

/* A failure on the display path, named once per (site, error). */
static void stdu_report_fail(const char *what, int rc)
{
	static const char *last_what;
	static int last_rc;
	static int budget = 12;

	if (what == last_what && rc == last_rc)
		return;
	if (budget <= 0)
		return;
	budget--;
	last_what = what;
	last_rc = rc;
	kprintf("[drm] vmwgfx: %s failed (%d)\n", what, rc);
}

/* Tell the host the guest wrote into this box of the display surface. */
static int stdu_update_image(struct vmw_device *v, int x1, int y1, int x2, int y2)
{
	SVGA3dCmdUpdateGBImage c;

	mm_memset(&c, 0, sizeof(c));
	c.image.sid = v->st_surface->sid;
	c.image.face = 0;
	c.image.mipmap = 0;
	c.box.x = (uint32_t)x1;
	c.box.y = (uint32_t)y1;
	c.box.z = 0;
	c.box.w = (uint32_t)(x2 - x1);
	c.box.h = (uint32_t)(y2 - y1);
	c.box.d = 1;
	return vmw_cmd_one_async(v, SVGA_3D_CMD_UPDATE_GB_IMAGE, &c, sizeof(c));
}

/* ...and that the target should show it. */
static int stdu_update_target(struct vmw_device *v, int x1, int y1, int x2, int y2)
{
	SVGA3dCmdUpdateGBScreenTarget c;

	mm_memset(&c, 0, sizeof(c));
	c.stid = VMW_STDU_ID;
	c.rect.x = (uint32_t)x1;
	c.rect.y = (uint32_t)y1;
	c.rect.w = (uint32_t)(x2 - x1);
	c.rect.h = (uint32_t)(y2 - y1);
	/* Recorded here rather than at the caller: this is the ONE command
	 * that costs the host a re-read, and every path that shows anything
	 * ends in it. */
	vmw_execbuf_note_update((uint64_t)c.rect.w * c.rect.h,
				c.rect.w == v->st_w && c.rect.h == v->st_h);
	/* Fire and forget.  This runs for every damage rectangle the server
	 * reports -- hundreds a second while a window is dragged -- and
	 * waiting for the host to finish each one put the whole desktop,
	 * cursor included, behind the device's execution time.  The device
	 * runs one context in submission order, so the update still lands
	 * after the rendering it is meant to show; a failure is named in the
	 * log when the buffer is collected.
	 *
	 * No fence either: a fence per rectangle was there to prod the host
	 * into consuming the batch, which submitting the buffer already
	 * does. */
	return vmw_cmd_one_async(v, SVGA_3D_CMD_UPDATE_GB_SCREENTARGET, &c,
				 sizeof(c));
}

/* Copy a rectangle of the framebuffer object into the display surface's MOB.
 *
 * Both sides are page arrays with their own pitches, so this walks rows and
 * splits each row at page boundaries on both ends. */
static int stdu_cpu_blit(struct vmw_device *v, struct drm_framebuffer *fb,
			 int x1, int y1, int x2, int y2)
{
	struct drm_gem_object *src = fb->obj;
	struct drm_gem_object *dst = v->st_bo;
	uint32_t bpp = fb->bpp / 8;
	uint32_t dst_pitch = v->st_w * 4;

	uint64_t t0 = timer_rdtsc();

	if (bpp != 4)
		return -EINVAL;
	vmw_execbuf_note_blit((uint64_t)(x2 - x1) * (y2 - y1));
	for (int y = y1; y < y2; y++) {
		uint64_t soff = fb->offset + (uint64_t)y * fb->pitch +
				(uint64_t)x1 * bpp;
		uint64_t doff = (uint64_t)y * dst_pitch + (uint64_t)x1 * 4;
		uint64_t bytes = (uint64_t)(x2 - x1) * bpp;

		while (bytes) {
			uint8_t *sp = drm_gem_page_virt(src, (uint32_t)(soff / PAGE_SIZE));
			uint8_t *dp = drm_gem_page_virt(dst, (uint32_t)(doff / PAGE_SIZE));
			if (!sp || !dp)
				return -EIO;
			uint32_t sin = (uint32_t)(soff % PAGE_SIZE);
			uint32_t din = (uint32_t)(doff % PAGE_SIZE);
			uint32_t chunk = PAGE_SIZE - (sin > din ? sin : din);
			if (chunk > bytes)
				chunk = (uint32_t)bytes;
			kmemcpy(dp + din, sp + sin, chunk);
			soff += chunk;
			doff += chunk;
			bytes -= chunk;
		}
	}
	vmw_execbuf_note_time(VMW_T_BLIT, timer_rdtsc() - t0);
	return 0;
}

/* Clip a rectangle to the target and to the framebuffer. */
static int stdu_clip(struct vmw_device *v, struct drm_framebuffer *fb, int *x1,
		     int *y1, int *x2, int *y2)
{
	if (*x1 < 0)
		*x1 = 0;
	if (*y1 < 0)
		*y1 = 0;
	if (*x2 > (int)v->st_w)
		*x2 = (int)v->st_w;
	if (*y2 > (int)v->st_h)
		*y2 = (int)v->st_h;
	if (fb) {
		if (*x2 > (int)fb->width)
			*x2 = (int)fb->width;
		if (*y2 > (int)fb->height)
			*y2 = (int)fb->height;
	}
	return (*x1 < *x2 && *y1 < *y2);
}

/* Can this surface be scanned out as it stands?
 *
 * What the screen target needs is an image its own size in a format it can
 * display.  It deliberately does NOT ask for SVGA3D_SURFACE_SCREENTARGET:
 * Mesa never sets that flag -- it asks for scan-out with the DRM flag and
 * leaves the device flag to the kernel -- so requiring it here sent every
 * client surface down the copy path, which is how a fully working X server
 * ended up drawing to a black screen. */
static int stdu_surface_direct(struct vmw_device *v, struct vmw_surface *s)
{
	return s && s->base_size.width == v->st_w &&
	       s->base_size.height == v->st_h &&
	       vmw_format_is_screen_target(s->format);
}

/* Copy from a client surface into the display surface.  Used when the
 * client's surface was not created for scan-out, or is a different size
 * from the mode -- a compositor's back buffer during a resize, typically. */
static int stdu_surface_copy(struct vmw_device *v, struct vmw_surface *src,
			     int x1, int y1, int x2, int y2)
{
	struct {
		SVGA3dCmdSurfaceCopy c;
		SVGA3dCopyBox box;
	} __attribute__((packed)) cmd;

	mm_memset(&cmd, 0, sizeof(cmd));
	cmd.c.src.sid = src->sid;
	cmd.c.src.face = 0;
	cmd.c.src.mipmap = 0;
	cmd.c.dest.sid = v->st_surface->sid;
	cmd.c.dest.face = 0;
	cmd.c.dest.mipmap = 0;
	cmd.box.x = (uint32_t)x1;
	cmd.box.y = (uint32_t)y1;
	cmd.box.z = 0;
	cmd.box.w = (uint32_t)(x2 - x1);
	cmd.box.h = (uint32_t)(y2 - y1);
	cmd.box.d = 1;
	cmd.box.srcx = (uint32_t)x1;
	cmd.box.srcy = (uint32_t)y1;
	cmd.box.srcz = 0;
	return vmw_cmd_one_async(v, SVGA_3D_CMD_SURFACE_COPY, &cmd.c, sizeof(cmd));
}

/* Make sure a screen target of this size exists.  A guest-backed surface
 * has no other way onto the display -- the screen-object blit and the VRAM
 * copy both read guest pages, and a surface's pixels live host-side -- so if
 * a mode set left no target up, the first update brings one up rather than
 * falling through to a path that cannot show it. */
int vmw_stdu_ensure(struct vmw_device *v, uint32_t w, uint32_t h)
{
	if (v->st_defined && v->st_w == w && v->st_h == h)
		return 0;
	return stdu_define(v, w, h);
}

/* Show `fb', or the given rectangle of it, on the screen target.
 *
 * `full' asks for the whole framebuffer and (re)binds what is scanned out;
 * a dirty-rectangle update passes full = 0 and only refreshes that box. */
static int vmw_stdu_present_do(struct vmw_device *v, struct drm_framebuffer *fb,
			       int x1, int y1, int x2, int y2, int full)
{
	/* One present is one frame, which is what the execbuf counters are
	 * reported against. */
	vmw_execbuf_note_frame();

	int rc;

	if (!v->st_defined || !fb || !fb->obj)
		return -ENODEV;
	if (!stdu_clip(v, fb, &x1, &y1, &x2, &y2))
		return 0;

	if (fb->obj->kind == DRM_GEM_SURFACE) {
		struct vmw_surface *s = fb->obj->priv;

		if (!s)
			return -ENODEV;
		int shown = 0;

		if (stdu_surface_direct(v, s)) {
			/* The client's own surface is the scan-out buffer:
			 * nothing is copied at all. */
			rc = 0;
			/* A full present (mode set, page flip) rebinds
			 * unconditionally: it is exactly the moment the
			 * scan-out buffer may have been replaced by a
			 * different surface that happens to hold the same
			 * id. */
			/* Bind only when the target is not already showing
			 * THIS surface.  A full present no longer forces it:
			 * what a full present means is that the whole area
			 * has to be re-sent, which is the update below, not
			 * that the binding is stale. */
			if (v->st_bound_sid != s->sid ||
			    v->st_bound_obj != (const void *)s) {
				rc = stdu_bind(v, s->sid);
				if (rc == 0)
					v->st_bound_obj = (const void *)s;
			}
			if (rc == 0)
				rc = stdu_update_target(v, x1, y1, x2, y2);
			/* If the device would not take it, fall through to the
			 * copy rather than failing: an error returned from a
			 * dirty-rectangle update makes the X server
			 * *permanently* unregister its damage tracking and
			 * stop asking for updates at all -- one refusal and
			 * the screen never changes again. */
			shown = (rc == 0);
			if (shown)
				stdu_report_path(v, "direct scan-out", s);
			else
				stdu_report_fail("direct scan-out", rc);
		}
		if (!shown) {
			stdu_report_path(v, "surface copy", s);
			/* SURFACE_COPY moves pixels, it does not convert
			 * them: the device requires both surfaces to share a
			 * format, so the display surface is built to match. */
			int fresh = stdu_display_surface_fmt(v, v->st_w, v->st_h,
							     s->format);
			if (fresh < 0) {
				stdu_report_fail("display surface", fresh);
				return fresh;
			}
			if (v->st_bound_sid != v->st_surface->sid ||
			    v->st_bound_obj != (const void *)v->st_surface) {
				rc = stdu_bind(v, v->st_surface->sid);
				if (rc) {
					stdu_report_fail("bind display surface", rc);
					return rc;
				}
				v->st_bound_obj = (const void *)v->st_surface;
			}
			/* A display surface the host has only just defined
			 * holds no image.  Copying the dirty rectangle into
			 * it would leave every other pixel blank -- and X
			 * redraws only what IT thinks changed, so the rest
			 * would stay blank for the life of the session: a
			 * black screen carrying the last few damaged
			 * rectangles.  The first update after a new surface
			 * therefore covers the whole framebuffer. */
			if (fresh) {
				x1 = 0;
				y1 = 0;
				x2 = (int)v->st_w;
				y2 = (int)v->st_h;
				if (!stdu_clip(v, fb, &x1, &y1, &x2, &y2))
					return 0;
			}
			rc = stdu_surface_copy(v, s, x1, y1, x2, y2);
			if (rc == 0)
				rc = stdu_update_target(v, x1, y1, x2, y2);
			else
				stdu_report_fail("surface copy", rc);
		}
		vmw_cmd_flush(v);
		return rc;
	}

	/* A buffer object: the guest wrote the pixels, so they are copied
	 * into the display surface's MOB and the host is told about the box. */
	stdu_report_path(v, "guest pixels", NULL);
	int fresh = stdu_display_surface(v, v->st_w, v->st_h);
	if (fresh < 0) {
		stdu_report_fail("display surface", fresh);
		return fresh;
	}
	if (v->st_bound_sid != v->st_surface->sid ||
	    v->st_bound_obj != (const void *)v->st_surface) {
		rc = stdu_bind(v, v->st_surface->sid);
		if (rc) {
			stdu_report_fail("bind display surface", rc);
			return rc;
		}
		v->st_bound_obj = (const void *)v->st_surface;
	}
	/* As above: a surface that has just been defined shows nothing until
	 * something has been put in all of it. */
	if (fresh) {
		x1 = 0;
		y1 = 0;
		x2 = (int)v->st_w;
		y2 = (int)v->st_h;
		if (!stdu_clip(v, fb, &x1, &y1, &x2, &y2))
			return 0;
	}
	rc = stdu_cpu_blit(v, fb, x1, y1, x2, y2);
	if (rc) {
		stdu_report_fail("guest-pixel blit", rc);
		return rc;
	}
	rc = stdu_update_image(v, x1, y1, x2, y2);
	if (rc) {
		stdu_report_fail("update image", rc);
		return rc;
	}
	rc = stdu_update_target(v, x1, y1, x2, y2);
	if (rc)
		stdu_report_fail("update target", rc);
	/* A finished update has to reach the display rather than wait for
	 * whatever is submitted next. */
	vmw_cmd_flush(v);
	return rc;
}

/* The present, timed.
 *
 * Every path out of it is included -- the copies, the command building and
 * the flush that hands the batch over -- because the question this answers
 * is how much of a frame the display path costs the guest, and a breakdown
 * that stopped at the interesting half would answer a different one. */
int vmw_stdu_present(struct vmw_device *v, struct drm_framebuffer *fb, int x1,
		     int y1, int x2, int y2, int full)
{
	uint64_t t0 = timer_rdtsc();
	int rc = vmw_stdu_present_do(v, fb, x1, y1, x2, y2, full);

	vmw_execbuf_note_time(VMW_T_PRESENT, timer_rdtsc() - t0);
	return rc;
}

/* Set the mode: define the target, then show the framebuffer on it. */
int vmw_stdu_set_mode(struct vmw_device *v, uint32_t w, uint32_t h,
		      struct drm_framebuffer *fb)
{
	int rc = stdu_define(v, w, h);

	if (rc)
		return rc;
	if (!fb)
		return 0;
	return vmw_stdu_present(v, fb, 0, 0, (int)w, (int)h, 1);
}
