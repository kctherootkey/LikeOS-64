// LikeOS -- vmwgfx: scan-out through screen targets.
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
// Display units.  Each screen target is one unit, its target id the unit
// number, unit 0 the primary.  A unit has its own size, its place in the
// host's desktop (xRoot/yRoot), the corner of the framebuffer it shows, what
// is bound to it, and its own display surface.  The per-unit entry points
// (vmw_stdu_unit_*) take the unit; the older single-head entry points are
// unit 0.  The device takes targets up to SVGA_REG_SCREENTARGET_MAX_WIDTH x
// _HEIGHT and several of them; VMW_STDU_MAX_UNITS is how many are driven.
//
// Unit 0's state is also published in the st_* fields of struct
// vmw_device, which code outside this file reads (never writes); the units
// themselves live in struct vmw_stdu_state, below.
//
// Besides showing a framebuffer, two client operations go through here:
// PRESENT copies rectangles of a surface onto the screen
// (vmw_kms_stdu_surface_dirty()), PRESENT_READBACK copies the screen back
// into a buffer-object framebuffer (vmw_kms_stdu_readback()).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2014-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

/* The display surface's pixel format for a 32-bit buffer object.  X8R8G8B8
 * is what the console and every dumb buffer here already are, so a scan-out
 * of one is a copy and not a conversion. */
#define VMW_STDU_FORMAT SVGA3D_X8R8G8B8

/* Display units driven.  The object table has room for
 * VMW_NUM_SCREENTARGETS targets; eight heads is what the driver offers. */
#define VMW_STDU_MAX_UNITS 8

/* Shrink guest-pixel updates to what actually changed.
 *
 * The buffer-object path copies the client's pixels into the display
 * surface's MOB and then tells the host to re-read a box of it
 * (UPDATE_GB_IMAGE) and show it (UPDATE_GB_SCREENTARGET).  With this on,
 * the copy compares as it goes (vmw_diff_memcpy()) and the box sent is the
 * bounding box of the pixels that differ -- nothing at all when none do.
 * That is only sound while the MOB holds exactly what the host holds; see
 * vmw_stdu_unit.cpu_valid for when it does.  0 restores the plain copy of
 * the whole damaged rectangle. */
#define VMW_STDU_CPU_DIFF 1

/* One screen target and what the driver knows about it. */
struct vmw_stdu_unit {
	int index;			/* the target id */
	int defined;
	uint32_t w, h;
	/* Where the host places this target in its desktop (xRoot/yRoot). */
	int32_t gui_x, gui_y;
	/* The framebuffer pixel shown at the target's top-left: rectangles
	 * arrive in framebuffer coordinates and are moved by this. */
	int32_t crtc_x, crtc_y;
	uint32_t bound_sid;		/* what the target currently shows */
	/* WHICH surface that id belonged to.
	 *
	 * The id alone is not identity: ids are reused, so a destroyed
	 * surface's number can come back attached to a different one, and
	 * a target left bound to the old meaning shows the wrong thing.
	 * Rebinding on every full present avoided that by never trusting the
	 * id -- at the cost of a BIND_GB_SCREENTARGET per page flip, which
	 * the display server issues once a frame, and which makes the host
	 * re-establish the scan-out every time.  Keeping the object as well
	 * settles identity properly and lets the bind be skipped when
	 * nothing has actually changed. */
	const void *bound_obj;
	struct vmw_surface *surface;	/* the driver's display surface */
	struct drm_gem_object *bo;	/* its MOB backing */
	/* The display surface's MOB holds exactly what the host holds, over
	 * the area the guest-pixel path writes.
	 *
	 * The diff copy compares the client's pixels against the MOB and
	 * tells the host only about what differs, so it is right only while
	 * the MOB is a true copy of the host's image.  That stops being so
	 * when the host writes the surface itself (a SURFACE_COPY into it, a
	 * present), when the surface is new, and when the device may have
	 * dropped an update (a refused command buffer: `cb_errors_at' is the
	 * device's refusal count when the copy was last known good).  A
	 * full plain refresh, or a readback, makes it true again. */
	int cpu_valid;
	uint32_t cb_errors_at;
	/* The framebuffer last shown, for operations addressed to "every unit
	 * showing this framebuffer".  Identity by pointer and id; never
	 * dereferenced. */
	const struct drm_framebuffer *fb;
	uint32_t fb_id;
	/* For stdu_report_path(). */
	const char *last_path;
};

struct vmw_stdu_state {
	struct vmw_stdu_unit unit[VMW_STDU_MAX_UNITS];
};

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
	 * display path: the display unit follows from that capability alone
	 * (SVGA_CAP_GBOBJECTS), and SVGA_REG_SCREENTARGET_MAX_WIDTH is read
	 * only for size limits.
	 *
	 * Requiring that register to be non-zero here was a mistake with
	 * teeth: where it reads 0 the screen target was switched off, and a
	 * guest-backed surface then fell to the screen-object blit, which
	 * cannot show one -- it takes the command, answers success and
	 * displays nothing.  Every ioctl passed, no error was logged
	 * anywhere, and X drew a fully working desktop onto a black screen. */
	return v->has_gb && v->otables_ready && !v->st_refused;
}

/* ---- units ---------------------------------------------------------------- */

/* Unit 0 as the rest of the driver sees it: the st_* fields of the device.
 * Called after every change to a unit; only unit 0 is published. */
static void stdu_publish(struct vmw_device *v, const struct vmw_stdu_unit *u)
{
	if (u->index != 0)
		return;
	v->st_defined = u->defined;
	v->st_w = u->w;
	v->st_h = u->h;
	v->st_bound_sid = u->bound_sid;
	v->st_bound_obj = u->bound_obj;
	v->st_surface = u->surface;
	v->st_bo = u->bo;
}

/* The unit, if the units have been set up at all; never allocates. */
static struct vmw_stdu_unit *stdu_unit_peek(struct vmw_device *v, int unit)
{
	if (!v->stdu || unit < 0 || unit >= VMW_STDU_MAX_UNITS)
		return NULL;
	return &v->stdu->unit[unit];
}

/* The unit, setting the units up on first use.  NULL for a unit number out
 * of range or when the state cannot be allocated. */
static struct vmw_stdu_unit *stdu_unit(struct vmw_device *v, int unit)
{
	BUILD_BUG_ON(VMW_STDU_MAX_UNITS > VMW_NUM_SCREENTARGETS);

	if (unit < 0 || unit >= VMW_STDU_MAX_UNITS)
		return NULL;
	if (!v->stdu) {
		struct vmw_stdu_state *st = kalloc(sizeof(*st));

		if (!st)
			return NULL;
		mm_memset(st, 0, sizeof(*st));
		for (int i = 0; i < VMW_STDU_MAX_UNITS; i++) {
			st->unit[i].index = i;
			st->unit[i].bound_sid = SVGA3D_INVALID_ID;
		}
		v->stdu = st;
	}
	return &v->stdu->unit[unit];
}

/* Bytes per pixel of a display-surface format, 0 for one this file does not
 * copy with the processor. */
static uint32_t stdu_format_cpp(uint32_t format)
{
	switch (format) {
	case SVGA3D_X8R8G8B8:
	case SVGA3D_A8R8G8B8:
	case SVGA3D_R8G8B8A8_UNORM:
	case SVGA3D_B8G8R8A8_UNORM:
	case SVGA3D_B8G8R8X8_UNORM:
		return 4;
	case SVGA3D_R5G6B5:
		return 2;
	case SVGA3D_P8:
		return 1;
	default:
		return 0;
	}
}

/* The display-surface format for a buffer object of this many bytes per
 * pixel, 0 for none. */
static uint32_t stdu_bo_format(uint32_t cpp)
{
	switch (cpp) {
	case 4:
		return VMW_STDU_FORMAT;
	case 2:
		return SVGA3D_R5G6B5;
	case 1:
		return SVGA3D_P8;
	default:
		return 0;
	}
}

static void stdu_drop_surface(struct vmw_device *v, struct vmw_stdu_unit *u)
{
	/* vmw_surface_destroy() owns everything here: it sends the destroy
	 * command, drops the backup buffer's reference and frees the surface
	 * itself.  So the MOB is only forgotten, never put a second time. */
	if (u->surface) {
		struct vmw_surface *s = u->surface;

		u->surface = NULL;
		vmw_surface_destroy(v, s);
	}
	u->bo = NULL;
	u->cpu_valid = 0;
	stdu_publish(v, u);
}

/* Bind image `sid' to the target (SVGA3D_INVALID_ID unbinds it). */
static int stdu_bind(struct vmw_device *v, struct vmw_stdu_unit *u,
		     uint32_t sid)
{
	SVGA3dCmdBindGBScreenTarget c;

	mm_memset(&c, 0, sizeof(c));
	c.stid = (uint32_t)u->index;
	c.image.sid = sid;
	c.image.face = 0;
	c.image.mipmap = 0;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_BIND_GB_SCREENTARGET, &c, sizeof(c));
	if (rc == 0) {
		u->bound_sid = sid;
		u->bound_obj = NULL;   /* the caller records identity */
		stdu_publish(v, u);
	}
	return rc;
}

static void stdu_unit_teardown(struct vmw_device *v, struct vmw_stdu_unit *u)
{
	if (!u->defined)
		return;
	if (u->bound_sid != SVGA3D_INVALID_ID)
		stdu_bind(v, u, SVGA3D_INVALID_ID);
	SVGA3dCmdDestroyGBScreenTarget c;
	c.stid = (uint32_t)u->index;
	vmw_cmd_one(v, SVGA_3D_CMD_DESTROY_GB_SCREENTARGET, &c, sizeof(c));
	u->defined = 0;
	u->w = u->h = 0;
	u->bound_sid = SVGA3D_INVALID_ID;
	u->fb = NULL;
	u->fb_id = 0;
	stdu_drop_surface(v, u);
	stdu_publish(v, u);
}

/* Define the target at this size and place, replacing whatever was there.
 * Unit 0 is the primary target; the others are not. */
static int stdu_define(struct vmw_device *v, struct vmw_stdu_unit *u,
		       uint32_t w, uint32_t h, int32_t gui_x, int32_t gui_y)
{
	if (u->defined && u->w == w && u->h == h && u->gui_x == gui_x &&
	    u->gui_y == gui_y)
		return 0;
	stdu_unit_teardown(v, u);

	SVGA3dCmdDefineGBScreenTarget c;
	mm_memset(&c, 0, sizeof(c));
	c.stid = (uint32_t)u->index;
	c.width = w;
	c.height = h;
	c.xRoot = gui_x;
	c.yRoot = gui_y;
	c.flags = (u->index == 0) ? SVGA_STFLAG_PRIMARY : 0;
	c.dpi = 0;
	int rc = vmw_cmd_one(v, SVGA_3D_CMD_DEFINE_GB_SCREENTARGET, &c, sizeof(c));
	if (rc)
		return rc;
	u->defined = 1;
	u->w = w;
	u->h = h;
	u->gui_x = gui_x;
	u->gui_y = gui_y;
	u->bound_sid = SVGA3D_INVALID_ID;
	stdu_publish(v, u);
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
static int stdu_display_surface_fmt(struct vmw_device *v,
				    struct vmw_stdu_unit *u, uint32_t w,
				    uint32_t h, uint32_t format)
{
	if (u->surface && u->surface->base_size.width == w &&
	    u->surface->base_size.height == h &&
	    u->surface->format == format)
		return 0;
	stdu_drop_surface(v, u);

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
	vmw_surface_res_init(v, s);
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
	u->surface = s;
	u->bo = s->backup;
	/* New: nothing the guest knows about it is known to be on the host
	 * yet.  The first guest-pixel update refreshes all of it. */
	u->cpu_valid = 0;
	stdu_publish(v, u);
	return 1;
}

/* Which path a present took, reported when it CHANGES.
 *
 * The change is the whole point: a silent switch from direct scan-out to the
 * copy path is the difference between a desktop and a black screen with the
 * last few damaged rectangles on it, and neither side of the switch fails or
 * logs anything by itself.  The budget is per event rather than one counter
 * for the file -- a shared one is spent by console flushes long before
 * startx, and the case worth reporting could then never report itself.
 * Per unit, too: two heads on different paths would otherwise report each
 * other's switches on every frame. */
static void stdu_report_path(struct vmw_device *v, struct vmw_stdu_unit *u,
			     const char *path, struct vmw_surface *s)
{
	(void)v;
	if (path == u->last_path)
		return;
	u->last_path = path;
	if (u->index != 0) {
		if (s)
			kprintf("[drm] vmwgfx: unit %d scan-out via %s: surface %u %ux%u fmt %u, target %ux%u\n",
				u->index, path, s->sid, s->base_size.width,
				s->base_size.height, s->format, u->w, u->h);
		else
			kprintf("[drm] vmwgfx: unit %d scan-out via %s: target %ux%u\n",
				u->index, path, u->w, u->h);
		return;
	}
	if (s)
		kprintf("[drm] vmwgfx: scan-out via %s: surface %u %ux%u fmt %u, target %ux%u\n",
			path, s->sid, s->base_size.width, s->base_size.height,
			s->format, u->w, u->h);
	else
		kprintf("[drm] vmwgfx: scan-out via %s: target %ux%u\n", path,
			u->w, u->h);
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
static int stdu_update_image(struct vmw_device *v, struct vmw_stdu_unit *u,
			     int x1, int y1, int x2, int y2)
{
	SVGA3dCmdUpdateGBImage c;

	mm_memset(&c, 0, sizeof(c));
	c.image.sid = u->surface->sid;
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

/* An UPDATE_GB_SCREENTARGET for a bounding box, written in place. */
struct vmw_stdu_update {
	SVGA3dCmdHeader header;
	SVGA3dCmdUpdateGBScreenTarget body;
} __attribute__((packed));

static void vmw_stdu_populate_update(void *cmd, int unit, s32 left, s32 right,
				     s32 top, s32 bottom)
{
	struct vmw_stdu_update *update = cmd;

	update->header.id = SVGA_3D_CMD_UPDATE_GB_SCREENTARGET;
	update->header.size = sizeof(update->body);

	update->body.stid = (uint32_t)unit;
	update->body.rect.x = left;
	update->body.rect.y = top;
	update->body.rect.w = (uint32_t)(right - left);
	update->body.rect.h = (uint32_t)(bottom - top);
}

/* ...and that the target should show it. */
static int stdu_update_target(struct vmw_device *v, struct vmw_stdu_unit *u,
			      int x1, int y1, int x2, int y2)
{
	SVGA3dCmdUpdateGBScreenTarget c;

	mm_memset(&c, 0, sizeof(c));
	c.stid = (uint32_t)u->index;
	c.rect.x = (uint32_t)x1;
	c.rect.y = (uint32_t)y1;
	c.rect.w = (uint32_t)(x2 - x1);
	c.rect.h = (uint32_t)(y2 - y1);
	/* Recorded here rather than at the caller: this is the ONE command
	 * that costs the host a re-read, and every path that shows anything
	 * ends in it. */
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

/* Copy a rectangle (unit coordinates) of the framebuffer object into the
 * display surface's MOB, with the copier `diff' carries.
 *
 * Both sides are page arrays with their own pitches; vmw_bo_cpu_blit()
 * walks rows and splits each row at page boundaries on both ends. */
static int stdu_cpu_blit(struct vmw_stdu_unit *u, struct drm_framebuffer *fb,
			 uint32_t cpp, int x1, int y1, int x2, int y2,
			 struct vmw_diff_cpy *diff)
{
	uint64_t dst_pitch = (uint64_t)u->w * cpp;
	uint64_t soff = (uint64_t)fb->offset +
			(uint64_t)(y1 + u->crtc_y) * fb->pitch +
			(uint64_t)(x1 + u->crtc_x) * cpp;
	uint64_t doff = (uint64_t)y1 * dst_pitch + (uint64_t)x1 * cpp;

	if (soff > U32_MAX || doff > U32_MAX || dst_pitch > U32_MAX)
		return -EINVAL;
	return vmw_bo_cpu_blit(u->bo, (u32)doff, (u32)dst_pitch, fb->obj,
			       (u32)soff, fb->pitch, (u32)(x2 - x1) * cpp,
			       (u32)(y2 - y1), diff);
}

/* Clip a rectangle (unit coordinates) to the target and to the part of the
 * framebuffer the unit shows. */
static int stdu_clip(struct vmw_stdu_unit *u, struct drm_framebuffer *fb,
		     int *x1, int *y1, int *x2, int *y2)
{
	if (*x1 < 0)
		*x1 = 0;
	if (*y1 < 0)
		*y1 = 0;
	if (*x2 > (int)u->w)
		*x2 = (int)u->w;
	if (*y2 > (int)u->h)
		*y2 = (int)u->h;
	if (fb) {
		if (*x2 > (int)fb->width - u->crtc_x)
			*x2 = (int)fb->width - u->crtc_x;
		if (*y2 > (int)fb->height - u->crtc_y)
			*y2 = (int)fb->height - u->crtc_y;
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
 * ended up drawing to a black screen.
 *
 * A unit that shows the framebuffer from an offset cannot bind it: the
 * target shows a surface from its origin. */
static int stdu_surface_direct(struct vmw_stdu_unit *u, struct vmw_surface *s)
{
	return s && s->base_size.width == u->w &&
	       s->base_size.height == u->h &&
	       u->crtc_x == 0 && u->crtc_y == 0 &&
	       vmw_format_is_screen_target(s->format);
}

/* Copy from a client surface into the display surface.  Used when the
 * client's surface was not created for scan-out, or is a different size
 * from the mode -- a compositor's back buffer during a resize, typically.
 * The box is in unit coordinates; the source is the same box moved by the
 * unit's corner of the framebuffer. */
static int stdu_surface_copy(struct vmw_device *v, struct vmw_stdu_unit *u,
			     struct vmw_surface *src, int x1, int y1, int x2,
			     int y2)
{
	struct {
		SVGA3dCmdSurfaceCopy c;
		SVGA3dCopyBox box;
	} __attribute__((packed)) cmd;

	mm_memset(&cmd, 0, sizeof(cmd));
	cmd.c.src.sid = src->sid;
	cmd.c.src.face = 0;
	cmd.c.src.mipmap = 0;
	cmd.c.dest.sid = u->surface->sid;
	cmd.c.dest.face = 0;
	cmd.c.dest.mipmap = 0;
	cmd.box.x = (uint32_t)x1;
	cmd.box.y = (uint32_t)y1;
	cmd.box.z = 0;
	cmd.box.w = (uint32_t)(x2 - x1);
	cmd.box.h = (uint32_t)(y2 - y1);
	cmd.box.d = 1;
	cmd.box.srcx = (uint32_t)(x1 + u->crtc_x);
	cmd.box.srcy = (uint32_t)(y1 + u->crtc_y);
	cmd.box.srcz = 0;
	int rc = vmw_cmd_one_async(v, SVGA_3D_CMD_SURFACE_COPY, &cmd.c,
				   sizeof(cmd));
	/* The host has written the display surface: its MOB no longer
	 * says what is on the screen. */
	u->cpu_valid = 0;
	return rc;
}

/* Make sure a screen target of this size exists.  A guest-backed surface
 * has no other way onto the display -- the screen-object blit and the VRAM
 * copy both read guest pages, and a surface's pixels live host-side -- so if
 * a mode set left no target up, the first update brings one up rather than
 * falling through to a path that cannot show it. */
int vmw_stdu_unit_ensure(struct vmw_device *v, int unit, uint32_t w,
			 uint32_t h)
{
	struct vmw_stdu_unit *u = stdu_unit(v, unit);

	if (!u)
		return unit < 0 || unit >= VMW_STDU_MAX_UNITS ? -EINVAL :
								 -ENOMEM;
	if (u->defined && u->w == w && u->h == h)
		return 0;
	return stdu_define(v, u, w, h, u->gui_x, u->gui_y);
}

int vmw_stdu_ensure(struct vmw_device *v, uint32_t w, uint32_t h)
{
	return vmw_stdu_unit_ensure(v, 0, w, h);
}

/* The guest-pixel path: the client wrote the pixels into a buffer object,
 * so they are copied into the display surface's MOB and the host is told
 * about the box.  The rectangle is in unit coordinates and clipped. */
static int stdu_present_bo(struct vmw_device *v, struct vmw_stdu_unit *u,
			   struct drm_framebuffer *fb, int x1, int y1, int x2,
			   int y2)
{
	uint32_t cpp = fb->bpp / 8;
	uint32_t format = stdu_bo_format(cpp);
	int rc;

	stdu_report_path(v, u, "guest pixels", NULL);
	if (!format) {
		stdu_report_fail("guest-pixel format", -EINVAL);
		return -EINVAL;
	}
	int fresh = stdu_display_surface_fmt(v, u, u->w, u->h, format);
	if (fresh < 0) {
		stdu_report_fail("display surface", fresh);
		return fresh;
	}
	int rebound = 0;
	if (u->bound_sid != u->surface->sid ||
	    u->bound_obj != (const void *)u->surface) {
		rc = stdu_bind(v, u, u->surface->sid);
		if (rc) {
			stdu_report_fail("bind display surface", rc);
			return rc;
		}
		u->bound_obj = (const void *)u->surface;
		stdu_publish(v, u);
		rebound = 1;
	}

	/* What the copy may skip: nothing, unless the MOB is a true copy of
	 * the host's image (see cpu_valid) and the target is still showing
	 * the image it showed before this call. */
	int resync = 0;
	int diffable = 0;

	if (VMW_STDU_CPU_DIFF) {
		resync = !u->cpu_valid || u->cb_errors_at != v->cb_errors;
		diffable = !fresh && !rebound && !resync;
	}

	/* As above: a surface that has just been defined shows nothing until
	 * something has been put in all of it.  The same goes for a display
	 * surface whose MOB is no longer known to match the host: the area
	 * the guest-pixel path covers is refreshed once in full, after which
	 * the copy can compare against it again. */
	if (fresh || resync) {
		x1 = 0;
		y1 = 0;
		x2 = (int)u->w;
		y2 = (int)u->h;
		if (!stdu_clip(u, fb, &x1, &y1, &x2, &y2))
			return 0;
	}

	if (diffable) {
		struct vmw_diff_cpy diff = VMW_CPU_BLIT_DIFF_INITIALIZER((int)cpp);

		rc = stdu_cpu_blit(u, fb, cpp, x1, y1, x2, y2, &diff);
		if (rc) {
			/* A partial copy leaves the MOB somewhere between
			 * the old and the new image. */
			u->cpu_valid = 0;
			stdu_report_fail("guest-pixel blit", rc);
			return rc;
		}
		if (diff.rect.x1 >= diff.rect.x2 || diff.rect.y1 >= diff.rect.y2) {
			/* Nothing changed: the screen already shows it. */
			vmw_cmd_flush(v);
			return 0;
		}
		/* The box of what differs, in the display surface -- which is
		 * unit coordinates. */
		x1 = diff.rect.x1;
		y1 = diff.rect.y1;
		x2 = diff.rect.x2;
		y2 = diff.rect.y2;
	} else {
		struct vmw_diff_cpy copy = VMW_CPU_BLIT_INITIALIZER;

		rc = stdu_cpu_blit(u, fb, cpp, x1, y1, x2, y2, &copy);
		if (rc) {
			u->cpu_valid = 0;
			stdu_report_fail("guest-pixel blit", rc);
			return rc;
		}
		if (fresh || resync) {
			u->cpu_valid = 1;
			u->cb_errors_at = v->cb_errors;
		}
	}
	rc = stdu_update_image(v, u, x1, y1, x2, y2);
	if (rc) {
		u->cpu_valid = 0;
		stdu_report_fail("update image", rc);
		return rc;
	}
	rc = stdu_update_target(v, u, x1, y1, x2, y2);
	if (rc)
		stdu_report_fail("update target", rc);
	/* A finished update has to reach the display rather than wait for
	 * whatever is submitted next. */
	vmw_cmd_flush(v);
	return rc;
}

/* Show `fb', or the given rectangle of it (framebuffer coordinates), on
 * one unit.
 *
 * `full' asks for the whole framebuffer and (re)binds what is scanned out;
 * a dirty-rectangle update passes full = 0 and only refreshes that box. */
static int vmw_stdu_present_do(struct vmw_device *v, struct vmw_stdu_unit *u,
			       struct drm_framebuffer *fb, int x1, int y1,
			       int x2, int y2, int full)
{
	int rc;

	/* Whether the call covers the whole framebuffer does not change
	 * what is done: binding is decided by identity below, and an update
	 * covers exactly the rectangle it was given. */
	(void)full;
	if (!u->defined || !fb || !fb->obj)
		return -ENODEV;
	/* What the unit shows from here on, for operations addressed to
	 * every unit showing a framebuffer. */
	u->fb = fb;
	u->fb_id = fb->id;
	x1 -= u->crtc_x;
	x2 -= u->crtc_x;
	y1 -= u->crtc_y;
	y2 -= u->crtc_y;
	if (!stdu_clip(u, fb, &x1, &y1, &x2, &y2))
		return 0;

	if (fb->obj->kind == DRM_GEM_SURFACE) {
		struct vmw_surface *s = fb->obj->priv;

		if (!s)
			return -ENODEV;
		int shown = 0;

		if (stdu_surface_direct(u, s)) {
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
			if (u->bound_sid != s->sid ||
			    u->bound_obj != (const void *)s) {
				rc = stdu_bind(v, u, s->sid);
				if (rc == 0) {
					u->bound_obj = (const void *)s;
					stdu_publish(v, u);
				}
			}
			if (rc == 0)
				rc = stdu_update_target(v, u, x1, y1, x2, y2);
			/* If the device would not take it, fall through to the
			 * copy rather than failing: an error returned from a
			 * dirty-rectangle update makes the X server
			 * *permanently* unregister its damage tracking and
			 * stop asking for updates at all -- one refusal and
			 * the screen never changes again. */
			shown = (rc == 0);
			if (shown)
				stdu_report_path(v, u, "direct scan-out", s);
			else
				stdu_report_fail("direct scan-out", rc);
		}
		if (!shown) {
			stdu_report_path(v, u, "surface copy", s);
			/* SURFACE_COPY moves pixels, it does not convert
			 * them: the device requires both surfaces to share a
			 * format, so the display surface is built to match. */
			int fresh = stdu_display_surface_fmt(v, u, u->w, u->h,
							     s->format);
			if (fresh < 0) {
				stdu_report_fail("display surface", fresh);
				return fresh;
			}
			if (u->bound_sid != u->surface->sid ||
			    u->bound_obj != (const void *)u->surface) {
				rc = stdu_bind(v, u, u->surface->sid);
				if (rc) {
					stdu_report_fail("bind display surface", rc);
					return rc;
				}
				u->bound_obj = (const void *)u->surface;
				stdu_publish(v, u);
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
				x2 = (int)u->w;
				y2 = (int)u->h;
				if (!stdu_clip(u, fb, &x1, &y1, &x2, &y2))
					return 0;
			}
			rc = stdu_surface_copy(v, u, s, x1, y1, x2, y2);
			if (rc == 0)
				rc = stdu_update_target(v, u, x1, y1, x2, y2);
			else
				stdu_report_fail("surface copy", rc);
		}
		vmw_cmd_flush(v);
		return rc;
	}

	return stdu_present_bo(v, u, fb, x1, y1, x2, y2);
}

/* The present, timed.
 *
 * Every path out of it is included -- the copies, the command building and
 * the flush that hands the batch over -- because the question this answers
 * is how much of a frame the display path costs the guest, and a breakdown
 * that stopped at the interesting half would answer a different one. */
int vmw_stdu_unit_present(struct vmw_device *v, int unit,
			  struct drm_framebuffer *fb, int x1, int y1, int x2,
			  int y2, int full)
{
	struct vmw_stdu_unit *u = stdu_unit_peek(v, unit);

	if (!u)
		return -ENODEV;
	int rc = vmw_stdu_present_do(v, u, fb, x1, y1, x2, y2, full);

	return rc;
}

int vmw_stdu_present(struct vmw_device *v, struct drm_framebuffer *fb, int x1,
		     int y1, int x2, int y2, int full)
{
	return vmw_stdu_unit_present(v, 0, fb, x1, y1, x2, y2, full);
}

/* Set the mode of one unit: define the target, then show the framebuffer
 * on it. */
int vmw_stdu_unit_set_mode(struct vmw_device *v, int unit, uint32_t w,
			   uint32_t h, int32_t gui_x, int32_t gui_y,
			   int32_t crtc_x, int32_t crtc_y,
			   struct drm_framebuffer *fb)
{
	struct vmw_stdu_unit *u = stdu_unit(v, unit);

	if (!u)
		return unit < 0 || unit >= VMW_STDU_MAX_UNITS ? -EINVAL :
								 -ENOMEM;
	int rc = stdu_define(v, u, w, h, gui_x, gui_y);

	if (rc)
		return rc;
	u->crtc_x = crtc_x;
	u->crtc_y = crtc_y;
	if (!fb)
		return 0;
	return vmw_stdu_present_do(v, u, fb, crtc_x, crtc_y,
				   crtc_x + (int)w, crtc_y + (int)h, 1);
}

int vmw_stdu_set_mode(struct vmw_device *v, uint32_t w, uint32_t h,
		      struct drm_framebuffer *fb)
{
	return vmw_stdu_unit_set_mode(v, 0, w, h, 0, 0, 0, 0, fb);
}

void vmw_stdu_unit_teardown(struct vmw_device *v, int unit)
{
	struct vmw_stdu_unit *u = stdu_unit_peek(v, unit);

	if (u)
		stdu_unit_teardown(v, u);
}

void vmw_stdu_teardown(struct vmw_device *v)
{
	vmw_stdu_unit_teardown(v, 0);
}

int vmw_stdu_unit_defined(struct vmw_device *v, int unit, uint32_t *w,
			  uint32_t *h)
{
	struct vmw_stdu_unit *u = stdu_unit_peek(v, unit);

	if (!u || !u->defined)
		return 0;
	if (w)
		*w = u->w;
	if (h)
		*h = u->h;
	return 1;
}

int vmw_stdu_num_units(struct vmw_device *v)
{
	(void)v;
	return VMW_STDU_MAX_UNITS;
}

/* Surface id `sid' is going back to the allocator.  Called from
 * vmw_surface_destroy(), which says why it matters.  Every unit: a surface
 * can be shown on several. */
void vmw_stdu_forget_sid(struct vmw_device *v, uint32_t sid)
{
	if (!v->stdu) {
		if (v->st_bound_sid == sid)
			v->st_bound_sid = SVGA3D_INVALID_ID;
		return;
	}
	for (int i = 0; i < VMW_STDU_MAX_UNITS; i++) {
		struct vmw_stdu_unit *u = &v->stdu->unit[i];

		if (u->bound_sid == sid) {
			u->bound_sid = SVGA3D_INVALID_ID;
			stdu_publish(v, u);
		}
	}
}

/* ---- client-driven presents and readbacks -------------------------------
 *
 * Both walk a list of clip rectangles over every unit they concern, the way
 * a dirty update is spread over the heads a framebuffer is shown on:
 * stdu_helper_dirty() moves each clip from framebuffer into unit
 * coordinates, drops the ones outside the unit, clips the rest to it and
 * hands each to the operation's clip() callback; once per unit,
 * fifo_commit() emits what the clips built.  When fifo_reserve_size is
 * non-zero, `cmd' is a command reservation of that size, zeroed, for the
 * callbacks to fill; fifo_commit() commits it (0 bytes cancels). */
struct stdu_kms_dirty {
	void (*fifo_commit)(struct stdu_kms_dirty *);
	void (*clip)(struct stdu_kms_dirty *);
	size_t fifo_reserve_size;
	struct vmw_device *v;
	struct vmw_stdu_unit *unit;
	void *cmd;
	u32 num_hits;
	s32 fb_x;
	s32 fb_y;
	s32 unit_x1;
	s32 unit_y1;
	s32 unit_x2;
	s32 unit_y2;
};

/**
 * struct vmw_stdu_dirty - closure structure for the update functions
 *
 * @base: The walk over units and clips; see stdu_helper_dirty().
 * @left: Left side of bounding box.
 * @right: Right side of bounding box.
 * @top: Top side of bounding box.
 * @bottom: Bottom side of bounding box.
 * @fb_left: Left side of the framebuffer/content bounding box
 * @fb_top: Top of the framebuffer/content bounding box
 * @pitch: framebuffer pitch (stride)
 * @fb: buffer-object framebuffer when copying between it and screen targets.
 * @sid: Surface ID when copying between surface and screen targets.
 * @rc: The first failure of a unit's commit, 0 for none.
 */
struct vmw_stdu_dirty {
	struct stdu_kms_dirty base;
	s32 left, right, top, bottom;
	s32 fb_left, fb_top;
	u32 pitch;
	struct drm_framebuffer *fb;
	u32 sid;
	int rc;
};

/* Does unit `u' show framebuffer `fb'?  Identity only. */
static int stdu_unit_shows(const struct vmw_stdu_unit *u,
			   const struct drm_framebuffer *fb)
{
	return u->defined && fb && u->fb == fb && u->fb_id == fb->id;
}

static int stdu_helper_dirty(struct vmw_device *v, struct drm_framebuffer *fb,
			     const struct drm_vmw_rect *vclips, s32 dest_x,
			     s32 dest_y, uint32_t num_clips, int increment,
			     int unit, struct stdu_kms_dirty *dirty)
{
	struct vmw_stdu_unit *units[VMW_STDU_MAX_UNITS];
	u32 num_units = 0;
	u32 i, k;

	dirty->v = v;

	/* A unit named by the caller is the only one; otherwise every unit
	 * that is showing this framebuffer. */
	if (unit != VMW_STDU_ALL_UNITS) {
		struct vmw_stdu_unit *u = stdu_unit_peek(v, unit);

		if (u && u->defined)
			units[num_units++] = u;
	} else if (v->stdu) {
		for (int j = 0; j < VMW_STDU_MAX_UNITS; j++)
			if (stdu_unit_shows(&v->stdu->unit[j], fb))
				units[num_units++] = &v->stdu->unit[j];
	}

	for (k = 0; k < num_units; k++) {
		struct vmw_stdu_unit *u = units[k];
		s32 crtc_x = u->crtc_x;
		s32 crtc_y = u->crtc_y;
		s32 crtc_width = (s32)u->w;
		s32 crtc_height = (s32)u->h;
		const struct drm_vmw_rect *vclips_ptr = vclips;

		dirty->unit = u;
		dirty->cmd = NULL;
		if (dirty->fifo_reserve_size > 0) {
			dirty->cmd = VMW_CMD_RESERVE(v,
				(uint32_t)dirty->fifo_reserve_size);
			if (!dirty->cmd)
				return -ENOMEM;

			mm_memset(dirty->cmd, 0, dirty->fifo_reserve_size);
		}
		dirty->num_hits = 0;
		for (i = 0; i < num_clips; i++, vclips_ptr += increment) {
			s32 clip_left;
			s32 clip_top;

			dirty->fb_x = vclips_ptr->x;
			dirty->fb_y = vclips_ptr->y;
			dirty->unit_x2 = dirty->fb_x + (s32)vclips_ptr->w +
				dest_x - crtc_x;
			dirty->unit_y2 = dirty->fb_y + (s32)vclips_ptr->h +
				dest_y - crtc_y;

			dirty->unit_x1 = dirty->fb_x + dest_x - crtc_x;
			dirty->unit_y1 = dirty->fb_y + dest_y - crtc_y;

			/* Skip this clip if it's outside the unit */
			if (dirty->unit_x1 >= crtc_width ||
			    dirty->unit_y1 >= crtc_height ||
			    dirty->unit_x2 <= 0 || dirty->unit_y2 <= 0)
				continue;

			/* Clip right and bottom to the unit */
			dirty->unit_x2 = min_t(s32, dirty->unit_x2,
					       crtc_width);
			dirty->unit_y2 = min_t(s32, dirty->unit_y2,
					       crtc_height);

			/* Clip left and top to the unit */
			clip_left = min_t(s32, dirty->unit_x1, 0);
			clip_top = min_t(s32, dirty->unit_y1, 0);
			dirty->unit_x1 -= clip_left;
			dirty->unit_y1 -= clip_top;
			dirty->fb_x -= clip_left;
			dirty->fb_y -= clip_top;

			dirty->clip(dirty);
		}

		dirty->fifo_commit(dirty);
	}

	return 0;
}

/* The image a unit shows, by device id: the driver's display surface or a
 * client surface bound directly.  SVGA3D_INVALID_ID when nothing is bound
 * -- including a surface that has been destroyed since, whose id
 * vmw_stdu_forget_sid() took back. */
static uint32_t stdu_shown_sid(const struct vmw_stdu_unit *u)
{
	return u->bound_sid;
}

/* An UPDATE_GB_SCREENTARGET preceded, when the source is not the image the
 * unit shows, by a SURFACE_COPY with one box per clip. */
struct vmw_stdu_surface_copy {
	SVGA3dCmdHeader header;
	SVGA3dCmdSurfaceCopy body;
} __attribute__((packed));

/**
 * vmw_kms_stdu_surface_clip - Callback to encode a surface copy command cliprect
 *
 * @dirty: The closure structure.
 *
 * Encodes a surface copy command cliprect and updates the bounding box
 * for the copy.
 */
static void vmw_kms_stdu_surface_clip(struct stdu_kms_dirty *dirty)
{
	struct vmw_stdu_dirty *sdirty =
		container_of(dirty, struct vmw_stdu_dirty, base);
	struct vmw_stdu_surface_copy *cmd = dirty->cmd;
	uint32_t shown = stdu_shown_sid(dirty->unit);

	if (shown == SVGA3D_INVALID_ID)
		return;

	if (sdirty->sid != shown) {
		SVGA3dCopyBox *blit = (SVGA3dCopyBox *)&cmd[1];

		blit += dirty->num_hits;
		blit->srcx = (uint32_t)dirty->fb_x;
		blit->srcy = (uint32_t)dirty->fb_y;
		blit->x = (uint32_t)dirty->unit_x1;
		blit->y = (uint32_t)dirty->unit_y1;
		blit->d = 1;
		blit->w = (uint32_t)(dirty->unit_x2 - dirty->unit_x1);
		blit->h = (uint32_t)(dirty->unit_y2 - dirty->unit_y1);
	}

	dirty->num_hits++;

	/* Destination bounding box */
	sdirty->left = min_t(s32, sdirty->left, dirty->unit_x1);
	sdirty->top = min_t(s32, sdirty->top, dirty->unit_y1);
	sdirty->right = max_t(s32, sdirty->right, dirty->unit_x2);
	sdirty->bottom = max_t(s32, sdirty->bottom, dirty->unit_y2);
}

/**
 * vmw_kms_stdu_surface_fifo_commit - Callback to fill in and submit a surface
 * copy command.
 *
 * @dirty: The closure structure.
 *
 * Fills in the missing fields in a surface copy command, and encodes a screen
 * target update command.
 */
static void vmw_kms_stdu_surface_fifo_commit(struct stdu_kms_dirty *dirty)
{
	struct vmw_stdu_dirty *sdirty =
		container_of(dirty, struct vmw_stdu_dirty, base);
	struct vmw_stdu_unit *u = dirty->unit;
	struct vmw_stdu_surface_copy *cmd = dirty->cmd;
	struct vmw_stdu_update *update;
	uint32_t shown = stdu_shown_sid(u);
	size_t blit_size = sizeof(SVGA3dCopyBox) * dirty->num_hits;
	size_t commit_size;

	if (!dirty->num_hits) {
		vmw_cmd_commit(dirty->v, 0);
		return;
	}

	if (sdirty->sid != shown) {
		SVGA3dCopyBox *blit = (SVGA3dCopyBox *)&cmd[1];

		cmd->header.id = SVGA_3D_CMD_SURFACE_COPY;
		cmd->header.size = (uint32_t)(sizeof(cmd->body) + blit_size);
		cmd->body.src.sid = sdirty->sid;
		cmd->body.dest.sid = shown;
		update = (struct vmw_stdu_update *)&blit[dirty->num_hits];
		commit_size = sizeof(*cmd) + blit_size + sizeof(*update);
		/* The host is writing the image the unit shows.  If that is
		 * the driver's display surface, its MOB stops being a copy
		 * of the screen. */
		if (u->surface && shown == u->surface->sid)
			u->cpu_valid = 0;
	} else {
		update = dirty->cmd;
		commit_size = sizeof(*update);
	}

	vmw_stdu_populate_update(update, u->index, sdirty->left,
				 sdirty->right, sdirty->top, sdirty->bottom);

	vmw_cmd_commit(dirty->v, (uint32_t)commit_size);

	sdirty->left = sdirty->top = S32_MAX;
	sdirty->right = sdirty->bottom = INT_MIN;
}

/**
 * vmw_kms_stdu_surface_dirty - Dirty part of a surface backed framebuffer
 *
 * @v: The device.
 * @fb: The framebuffer the update is for.
 * @clips: Array of clip rects, kernel memory, in @srf coordinates.
 * @srf: Pointer to surface to blit from. If NULL, the surface attached
 * to @fb will be used.
 * @dest_x: X coordinate offset to align @srf with framebuffer coordinates.
 * @dest_y: Y coordinate offset to align @srf with framebuffer coordinates.
 * @num_clips: Number of clip rects in @clips.
 * @inc: Increment to use when looping over @clips.
 * @out_fence: If non-NULL, will return a ref-counted pointer to a fence
 * for the update.  The returned fence pointer may be NULL in which case the
 * device has already synchronized.
 * @unit: The unit to update, or VMW_STDU_ALL_UNITS (-1) for every unit
 * showing @fb.
 *
 * Each unit concerned gets one SURFACE_COPY from @srf into the image it
 * shows, a box per clip, and one UPDATE_GB_SCREENTARGET of their bounding
 * box -- or only the update when @srf IS the image shown.
 *
 * Returns: %0 on success, negative error code on failure.
 */
int vmw_kms_stdu_surface_dirty(struct vmw_device *v, struct drm_framebuffer *fb,
			       struct drm_vmw_rect *clips,
			       struct vmw_surface *srf, int32_t dest_x,
			       int32_t dest_y, uint32_t num_clips, int inc,
			       struct drm_fence **out_fence, int unit)
{
	struct vmw_stdu_dirty sdirty;
	int ret;

	if (out_fence)
		*out_fence = NULL;
	if (!srf && fb && fb->obj && fb->obj->kind == DRM_GEM_SURFACE)
		srf = fb->obj->priv;
	if (!srf || (num_clips && !clips) || inc < 1)
		return -EINVAL;
	/* One reservation holds the copy and its boxes: bounded by what a
	 * reservation can be.  The arithmetic is done wide so that an absurd
	 * count is refused rather than wrapped. */
	uint64_t reserve = sizeof(struct vmw_stdu_surface_copy) +
			   (uint64_t)sizeof(SVGA3dCopyBox) * num_clips +
			   sizeof(struct vmw_stdu_update);
	if (reserve > U32_MAX)
		return -EINVAL;

	mm_memset(&sdirty, 0, sizeof(sdirty));
	sdirty.base.fifo_commit = vmw_kms_stdu_surface_fifo_commit;
	sdirty.base.clip = vmw_kms_stdu_surface_clip;
	sdirty.base.fifo_reserve_size = (size_t)reserve;
	sdirty.sid = srf->sid;
	sdirty.left = sdirty.top = S32_MAX;
	sdirty.right = sdirty.bottom = INT_MIN;

	ret = stdu_helper_dirty(v, fb, clips, dest_x, dest_y, num_clips, inc,
				unit, &sdirty.base);

	/* The update has to reach the display rather than wait for whatever
	 * is submitted next. */
	vmw_cmd_flush(v);
	if (ret == 0 && out_fence)
		*out_fence = vmw_fence_emit(v, DRM_VMW_FENCE_FLAG_EXEC);

	return ret;
}

/* Make the display surface's MOB a true copy of the host's image: read it
 * back and wait.  Needed before the processor reads the screen when the
 * host has written the surface since the guest last did. */
static int stdu_display_surface_readback(struct vmw_device *v,
					 struct vmw_stdu_unit *u)
{
	SVGA3dCmdReadbackGBSurface c;
	struct drm_fence *f;
	int rc;

	c.sid = u->surface->sid;
	rc = vmw_cmd_one(v, SVGA_3D_CMD_READBACK_GB_SURFACE, &c, sizeof(c));
	if (rc)
		return rc;
	f = vmw_fence_emit(v, DRM_VMW_FENCE_FLAG_EXEC);
	if (f) {
		rc = vmw_fence_wait(v, f, VMW_FENCE_TIMEOUT_NS);
		drm_fence_put(f);
	} else {
		vmw_cmd_drain(v);
	}
	if (rc == 0) {
		u->cpu_valid = 1;
		u->cb_errors_at = v->cb_errors;
	}
	return rc;
}

/**
 * vmw_stdu_bo_cpu_clip - Callback to encode a CPU blit
 *
 * @dirty: The closure structure.
 *
 * This function calculates the bounding box for all the incoming clips.
 */
static void vmw_stdu_bo_cpu_clip(struct stdu_kms_dirty *dirty)
{
	struct vmw_stdu_dirty *ddirty =
		container_of(dirty, struct vmw_stdu_dirty, base);

	dirty->num_hits = 1;

	/* Calculate destination bounding box */
	ddirty->left = min_t(s32, ddirty->left, dirty->unit_x1);
	ddirty->top = min_t(s32, ddirty->top, dirty->unit_y1);
	ddirty->right = max_t(s32, ddirty->right, dirty->unit_x2);
	ddirty->bottom = max_t(s32, ddirty->bottom, dirty->unit_y2);

	/*
	 * Calculate content bounding box.  We only need the top-left
	 * coordinate because width and height will be the same as the
	 * destination bounding box above
	 */
	ddirty->fb_left = min_t(s32, ddirty->fb_left, dirty->fb_x);
	ddirty->fb_top  = min_t(s32, ddirty->fb_top, dirty->fb_y);
}

/**
 * vmw_stdu_bo_cpu_commit - Callback to do a CPU blit from the screen into
 * the buffer object
 *
 * @dirty: The closure structure.
 *
 * Copies the bounding box of the clips out of the unit's display surface --
 * the image the guest-pixel path keeps in guest memory -- into the
 * framebuffer's buffer object.  A unit that is not showing its display
 * surface (a client surface scanned out directly) has no image in guest
 * memory to copy and is left alone.
 */
static void vmw_stdu_bo_cpu_commit(struct stdu_kms_dirty *dirty)
{
	struct vmw_stdu_dirty *ddirty =
		container_of(dirty, struct vmw_stdu_dirty, base);
	struct vmw_stdu_unit *u = dirty->unit;
	struct drm_framebuffer *fb = ddirty->fb;
	struct vmw_diff_cpy diff = VMW_CPU_BLIT_INITIALIZER;
	s32 width, height;
	u64 src_pitch, dst_pitch;
	u64 src_offset, dst_offset;
	uint32_t cpp;
	int rc;

	if (!dirty->num_hits)
		goto out;

	width = ddirty->right - ddirty->left;
	height = ddirty->bottom - ddirty->top;

	if (width <= 0 || height <= 0)
		goto out;

	if (!u->surface || !u->bo ||
	    u->bound_obj != (const void *)u->surface ||
	    u->bound_sid != u->surface->sid)
		goto out;
	cpp = stdu_format_cpp(u->surface->format);
	if (!cpp || cpp != fb->bpp / 8)
		goto out;

	if (!u->cpu_valid || u->cb_errors_at != dirty->v->cb_errors) {
		rc = stdu_display_surface_readback(dirty->v, u);
		if (rc) {
			if (!ddirty->rc)
				ddirty->rc = rc;
			goto out;
		}
	}

	/* From the display surface (guest copy of the screen)... */
	src_pitch = (u64)u->surface->base_size.width * cpp;
	src_offset = (u64)ddirty->top * src_pitch + (u64)ddirty->left * cpp;

	/* ...into the framebuffer's buffer object. */
	dst_pitch = fb->pitch;
	dst_offset = (u64)fb->offset + (u64)ddirty->fb_top * dst_pitch +
		     (u64)ddirty->fb_left * cpp;

	if (src_offset > U32_MAX || dst_offset > U32_MAX ||
	    src_pitch > U32_MAX) {
		if (!ddirty->rc)
			ddirty->rc = -EINVAL;
		goto out;
	}
	rc = vmw_bo_cpu_blit(fb->obj, (u32)dst_offset, (u32)dst_pitch,
			     u->bo, (u32)src_offset, (u32)src_pitch,
			     (u32)width * cpp, (u32)height, &diff);
	if (rc && !ddirty->rc)
		ddirty->rc = rc;
out:
	/* The next unit starts its own boxes. */
	ddirty->left = ddirty->top = S32_MAX;
	ddirty->right = ddirty->bottom = INT_MIN;
	ddirty->fb_left = ddirty->fb_top = S32_MAX;
}

/**
 * vmw_kms_stdu_readback - Perform a readback from the screen target system
 * into a buffer-object backed framebuffer.
 *
 * @v: The device.
 * @fp: The client, for the fence handle; may be NULL, and then
 * @user_fence_rep must be 0.
 * @fb: The buffer-object backed framebuffer.
 * @user_fence_rep: User-space struct drm_vmw_fence_rep address, 0 for none.
 * @clips: Array of clip rects, kernel memory, in framebuffer coordinates.
 * @num_clips: Number of clip rects in @clips.
 * @inc: Increment to use when looping over @clips.
 * @unit: The unit to read, or VMW_STDU_ALL_UNITS (-1) for every unit
 * showing @fb.
 *
 * Returns: %0 on success, negative error code on failure.
 */
int vmw_kms_stdu_readback(struct vmw_device *v, struct drm_file *fp,
			  struct drm_framebuffer *fb, uint64_t user_fence_rep,
			  struct drm_vmw_rect *clips, uint32_t num_clips,
			  int inc, int unit)
{
	struct vmw_stdu_dirty ddirty;
	int ret;

	/* Only a buffer-object framebuffer has memory to read the screen
	 * into. */
	if (!fb || !fb->obj || fb->obj->kind == DRM_GEM_SURFACE)
		return -EINVAL;
	if ((num_clips && !clips) || inc < 1)
		return -EINVAL;

	mm_memset(&ddirty, 0, sizeof(ddirty));
	ddirty.left = ddirty.top = S32_MAX;
	ddirty.right = ddirty.bottom = INT_MIN;
	ddirty.fb_left = ddirty.fb_top = S32_MAX;
	ddirty.pitch = fb->pitch;
	ddirty.fb = fb;

	ddirty.base.fifo_commit = vmw_stdu_bo_cpu_commit;
	ddirty.base.clip = vmw_stdu_bo_cpu_clip;
	ddirty.base.fifo_reserve_size = 0;

	ret = stdu_helper_dirty(v, fb, clips, 0, 0, num_clips, inc, unit,
				&ddirty.base);
	if (ret == 0)
		ret = ddirty.rc;

	/* The copy is done by the processor and is complete here, so the
	 * fence the client asked for only has to order it after everything
	 * submitted before. */
	if (user_fence_rep) {
		struct drm_vmw_fence_rep rep;
		struct drm_fence *fence = vmw_fence_emit(v,
						DRM_VMW_FENCE_FLAG_EXEC);

		mm_memset(&rep, 0, sizeof(rep));
		rep.error = ret;
		rep.fd = -1;
		if (fence) {
			uint32_t h = 0;

			if (fp && drm_fence_handle_create(fp, fence, &h) == 0)
				rep.handle = h;
			/* Zero, as for a submission: see the fence reply in
			 * vmw_execbuf.c for how the client reads `mask'. */
			rep.mask = 0;
			rep.seqno = fence->seqno;
			rep.passed_seqno = v->drm.fence_passed;
		} else if (!rep.error) {
			rep.error = -ENOMEM;
		}
		if (validate_user_ptr(user_fence_rep, sizeof(rep)))
			copy_to_user((void *)(uintptr_t)user_fence_rep, &rep,
				     sizeof(rep));
		if (fence)
			drm_fence_put(fence);
	}
	return ret;
}
