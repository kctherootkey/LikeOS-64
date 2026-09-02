// LikeOS-64 -- display-manager backend for the VMware SVGA II / SVGA3D
// device ("vmwgfx": the name userspace's driver stack looks for).
//
// Sits on the hardware primitives kernel/dev/video/vmsvga2.c exports.  Two
// display paths, chosen by what the host offers:
//   - screen objects: the scan-out buffer is a guest memory region (a
//     buffer object bound to a GMR) and the host reads it directly;
//     a dirty rectangle is one BLIT_GMRFB_TO_SCREEN command;
//   - legacy: the host scans out VRAM; dirty rectangles are copied from
//     the buffer object into VRAM and announced with SVGA_CMD_UPDATE.
// Fences come from the FIFO fence register; the fence interrupt signals
// them where the host has one, a short timer polls where it has not.
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/uapi/drm/drm_fourcc.h>
#include <kernel/uapi/ioctl.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

#define VMW_FENCE_TIMEOUT_NS 2000000000ULL

static struct vmw_device g_vmw;

/* vmw_surface.c / vmw_context.c */
long vmw_ioctl_gb_surface_create(struct vmw_device *v, struct drm_file *fp,
				 void *kb, int ext);
long vmw_ioctl_gb_surface_ref(struct vmw_device *v, struct drm_file *fp,
			      void *kb, int ext);
long vmw_ioctl_unref_surface(struct vmw_device *v, struct drm_file *fp, void *kb);
void vmw_surface_gem_free(struct vmw_device *v, struct drm_gem_object *o);
long vmw_ioctl_create_context(struct vmw_device *v, struct drm_file *fp,
			      int dx, int32_t *cid_out);
long vmw_ioctl_unref_context(struct vmw_device *v, struct drm_file *fp, uint32_t cid);
void vmw_file_release(struct vmw_device *v, struct drm_file *fp);
long vmw_ioctl_create_shader(struct vmw_device *v, struct drm_file *fp,
			     struct drm_vmw_shader_create_arg *a);
long vmw_ioctl_unref_shader(struct vmw_device *v, struct drm_file *fp, uint32_t shid);

/* ---- fences ---------------------------------------------------------- */

static void vmw_fence_events_check(void);

void vmw_fence_check(struct vmw_device *v)
{
	if (v->cb_ready) {
		/* Collecting the finished command buffers IS the fence check.
		 *
		 * A fence emitted through the command-buffer channel is a
		 * command inside one of those buffers, and it has passed when
		 * the buffer completes -- which is what cb_slot_completed()
		 * reports.  The device's fence REGISTER must not be consulted
		 * here: it belongs to the FIFO, and the console writes
		 * SVGA_CMD_FENCE into the FIFO out of the same counter these
		 * numbers come from.  The FIFO drains on its own, so that
		 * register runs ahead of anything a command-buffer context has
		 * executed, and reading it as "fences up to here have passed"
		 * signalled every client fence the moment it was created.
		 *
		 * Nothing failed when it did.  Every wait returned at once, so
		 * a client mapped a surface before the device had written it
		 * and read the frame before -- rendering that had plainly
		 * worked, arriving one frame late for ever.  Under X that is
		 * text and window content one repaint behind, which mostly
		 * means blank. */
		vmw_cmdbuf_poll(v);
	} else {
		/* No command buffers: the work IS the FIFO, and the FIFO's
		 * fence register is exactly the right answer. */
		uint32_t cur = vmsvga2_hw_fence_current();

		if (cur)
			drm_fence_signal_upto(&v->drm, cur);
	}
	vmw_fence_events_check();
}

static void vmw_fence_poll_fire(hrtimer_t *t)
{
	struct vmw_device *v = t->arg;

	vmw_fence_check(v);
	if (v->drm.fences)
		hrtimer_start_rel(&v->fence_poll, 2000000);
	else
		v->fence_poll_running = 0;
}

static void vmw_irq_cb(uint32_t status)
{
	(void)status;
	vmw_fence_check(&g_vmw);
}

/* Insert a fence after whatever was just submitted; returns it. */
struct drm_fence *vmw_fence_emit(struct vmw_device *v, uint32_t flags)
{
	uint32_t seq;

	/* Down the channel the work went.  With command buffers carrying the
	 * commands, a fence written to the FIFO is in a different queue from
	 * the batches it is supposed to follow and can pass while they are
	 * still running -- so the fence would promise completion that has
	 * not happened.  Command buffers take FIFO-format commands, and one
	 * context executes strictly in submission order. */
	if (v->cb_ready) {
		/* Allocated and submitted together: see vmw_cmd_fence_emit().
		 * Handing out the number first and submitting afterwards lets
		 * two threads swap, and then the higher number completes first
		 * and signals the lower -- whose work has not run. */
		seq = vmw_cmd_fence_emit(v);
	} else {
		seq = vmsvga2_fence_insert();
	}

	if (!seq) {
		/* No fence support (QEMU): everything completes in order
		 * with the doorbell, so a signalled fence is the truth. */
		vmsvga2_fifo_flush();
		return drm_fence_signalled(&v->drm);
	}
	v->drm.fence_seq = seq;
	struct drm_fence *f = drm_fence_create(&v->drm, seq, flags);
	if (f && !f->signaled) {
		if (v->hw.irq_enabled)
			vmsvga2_hw_set_fence_goal(seq);
		if (!v->fence_poll_running) {
			v->fence_poll_running = 1;
			hrtimer_start_rel(&v->fence_poll, 2000000);
		}
	}
	return f;
}

/* ---- buffer objects --------------------------------------------------- */

static int vmw_gem_init(struct drm_gem_object *o)
{
	struct vmw_bo *b = kalloc(sizeof(*b));

	if (!b)
		return -ENOMEM;
	mm_memset(b, 0, sizeof(*b));
	b->gmr_id = -1;
	b->mob.id = SVGA3D_INVALID_ID;
	o->priv = b;
	if (!o->pages)
		return 0;
	/* Guest-backed hosts: every buffer is a MOB (what surfaces bind to
	 * and what DX commands name).  A GMR as well where the host has
	 * them, for the 2D scan-out and legacy DMA paths. */
	if (g_vmw.has_gb && g_vmw.otables_ready) {
		int id = vmw_mob_alloc_id(&g_vmw);
		if (id >= 0) {
			b->mob.id = (uint32_t)id;
			if (vmw_mob_bind(&g_vmw, &b->mob, o->pages, o->npages,
					 (uint32_t)o->size) != 0) {
				vmw_mob_free_id(&g_vmw, (uint32_t)id);
				b->mob.id = SVGA3D_INVALID_ID;
			} else {
				o->backend_id = (uint32_t)id;
			}
		}
	}
	if (g_vmw.has_gmr && (b->mob.id == SVGA3D_INVALID_ID || o->scanout)) {
		int id = vmsvga2_gmr_alloc(o->npages);
		if (id >= 0 && vmsvga2_gmr_bind(id, o->pages, o->npages) == 0) {
			b->gmr_id = id;
			if (b->mob.id == SVGA3D_INVALID_ID)
				o->backend_id = (uint32_t)id;
		} else if (id >= 0) {
			vmsvga2_gmr_free(id);
		}
	}
	return 0;
}

static void vmw_gem_free(struct drm_gem_object *o)
{
	if (o->kind == DRM_GEM_SURFACE) {
		vmw_surface_gem_free(&g_vmw, o);
		return;
	}
	struct vmw_bo *b = o->priv;

	if (!b)
		return;
	if (b->mob.id != SVGA3D_INVALID_ID) {
		vmw_mob_unbind(&g_vmw, &b->mob);
		vmw_mob_free_id(&g_vmw, b->mob.id);
		b->mob.id = SVGA3D_INVALID_ID;
	}
	if (b->gmr_id >= 0)
		vmsvga2_gmr_free(b->gmr_id);
	kfree(b);
	o->priv = NULL;
}

/* The pages of every buffer object here are reachable by the host: they are
 * what a MOB page table names.  Hold them until it has finished with them. */
static void vmw_gem_release_pages(struct drm_gem_object *o)
{
	if (!o->pages)
		return;
	vmw_defer_free_pages(&g_vmw, o->pages, (uint32_t)o->npages);
}

static uint64_t vmw_gem_page_phys(struct drm_gem_object *o, uint64_t index)
{
	if (!o->pages || index >= o->npages)
		return 0;
	return o->pages[index];
}

/* ---- scan-out ---------------------------------------------------------- */

/* Define screen object 0.  With a buffer object the host scans the
 * object's GMR directly; with a surface (a rendered image, which lives
 * in host memory) the screen has no backing store and the image is
 * blitted onto it with BLIT_SURFACE_TO_SCREEN. */
static int vmw_define_screen(struct vmw_device *v, uint32_t w, uint32_t h,
			     struct drm_gem_object *o, uint32_t pitch)
{
	struct vmw_bo *b = (o && o->kind == DRM_GEM_BO) ? o->priv : NULL;
	uint32_t cmd[12];
	int backed = b && b->gmr_id >= 0;

	if (!backed && !(o && o->kind == DRM_GEM_SURFACE))
		return -ENODEV;
	cmd[0] = SVGA_CMD_DEFINE_SCREEN;
	cmd[1] = 11 * 4; /* structSize */
	cmd[2] = 0; /* id */
	cmd[3] = SVGA_SCREEN_MUST_BE_SET | SVGA_SCREEN_HAS_ROOT | (1 << 1) /* IS_PRIMARY */;
	cmd[4] = w;
	cmd[5] = h;
	cmd[6] = 0; /* root x */
	cmd[7] = 0; /* root y */
	cmd[8] = backed ? (uint32_t)b->gmr_id : SVGA_GMR_NULL;
	cmd[9] = 0; /* offset */
	cmd[10] = backed ? pitch : 0;
	cmd[11] = 0; /* cloneCount */
	if (vmw_cmd_raw(v, cmd, sizeof(cmd), 1) != 0)
		return -EIO;
	v->screen_defined = 1;
	v->screen_w = w;
	v->screen_h = h;
	v->scan_gmr = backed ? (uint32_t)b->gmr_id : SVGA_GMR_NULL;
	return 0;
}

/* A rendered surface onto the screen: one 3D command. */
static int vmw_surface_blit_screen(struct vmw_device *v, struct drm_gem_object *o,
				   int x1, int y1, int x2, int y2)
{
	struct vmw_surface *s = o->priv;
	struct {
		SVGA3dCmdHeader h;
		SVGA3dCmdBlitSurfaceToScreen b;
	} __attribute__((packed)) cmd;

	if (!s)
		return -ENODEV;
	cmd.h.id = SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN;
	cmd.h.size = sizeof(cmd.b);
	cmd.b.srcImage.sid = s->sid;
	cmd.b.srcImage.face = 0;
	cmd.b.srcImage.mipmap = 0;
	cmd.b.srcRect.left = x1;
	cmd.b.srcRect.top = y1;
	cmd.b.srcRect.right = x2;
	cmd.b.srcRect.bottom = y2;
	cmd.b.destScreenId = 0;
	cmd.b.destRect = cmd.b.srcRect;
	int rc = vmw_cmd_submit(v, &cmd, sizeof(cmd), SVGA3D_INVALID_ID);
	if (rc == 0) {
		/* Let the host consume it promptly. */
		struct drm_fence *f = vmw_fence_emit(v, 0);
		if (f)
			drm_fence_put(f);
	}
	return rc;
}

static int fb_is_surface(struct drm_framebuffer *fb)
{
	return fb->obj && fb->obj->kind == DRM_GEM_SURFACE;
}

/* A buffer object the host can scan out of directly: bound to a guest
 * memory region, and the image starts where the region does. */
static int fb_is_gmr_scanout(struct drm_framebuffer *fb)
{
	return !fb_is_surface(fb) && fb->obj && fb->obj->priv &&
	       ((struct vmw_bo *)fb->obj->priv)->gmr_id >= 0 &&
	       fb->offset == 0;
}

/* One screen blit.  `ring' announces it to the host; a caller queueing a run
 * of them passes 0 and rings once at the end (see vmw_fb_dirty). */
static int vmw_sou_blit_ex(struct vmw_device *v, struct drm_gem_object *o,
			   uint32_t pitch, int x1, int y1, int x2, int y2,
			   int ring)
{
	struct vmw_bo *b = o->priv;
	uint32_t cmd[5 + 8];

	if (!b || b->gmr_id < 0)
		return -ENODEV;
	cmd[0] = SVGA_CMD_DEFINE_GMRFB;
	cmd[1] = (uint32_t)b->gmr_id;
	cmd[2] = 0;
	cmd[3] = pitch;
	cmd[4] = 32 | (24 << 8);
	cmd[5] = SVGA_CMD_BLIT_GMRFB_TO_SCREEN;
	cmd[6] = (uint32_t)x1;
	cmd[7] = (uint32_t)y1;
	cmd[8] = (uint32_t)x1;
	cmd[9] = (uint32_t)y1;
	cmd[10] = (uint32_t)x2;
	cmd[11] = (uint32_t)y2;
	cmd[12] = 0; /* screen id */
	/* Down the driver's own channel, not straight into the FIFO: this
	 * blit shows a region the command-buffer channel has been filling,
	 * and two streams have no order between them.  See vmw_cmd_raw(). */
	return vmw_cmd_raw(v, cmd, sizeof(cmd), ring);
}

static int vmw_sou_blit(struct vmw_device *v, struct drm_gem_object *o,
			uint32_t pitch, int x1, int y1, int x2, int y2)
{
	return vmw_sou_blit_ex(v, o, pitch, x1, y1, x2, y2, 1);
}

/* Legacy: copy rows of the object into VRAM, then announce them. */
static int vmw_ldu_copy(struct vmw_device *v, struct drm_framebuffer *fb,
			int x1, int y1, int x2, int y2)
{
	struct drm_gem_object *o = fb->obj;
	uint32_t bpp = fb->bpp / 8;

	if (o->kind != DRM_GEM_BO)
		return -ENODEV;

	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > (int)v->hw.width)
		x2 = (int)v->hw.width;
	if (y2 > (int)v->hw.height)
		y2 = (int)v->hw.height;
	if (x2 > (int)fb->width)
		x2 = (int)fb->width;
	if (y2 > (int)fb->height)
		y2 = (int)fb->height;
	if (x1 >= x2 || y1 >= y2)
		return 0;
	uint8_t *vram = v->hw.fb_virt + v->hw.fb_offset;
	for (int y = y1; y < y2; y++) {
		uint64_t src_off = fb->offset + (uint64_t)y * fb->pitch + (uint64_t)x1 * bpp;
		uint64_t bytes = (uint64_t)(x2 - x1) * bpp;
		uint8_t *dst = vram + (uint64_t)y * v->hw.pitch + (uint64_t)x1 * bpp;
		/* The object is page-backed; a row may cross pages. */
		while (bytes) {
			uint32_t page = (uint32_t)(src_off / PAGE_SIZE);
			uint32_t in = (uint32_t)(src_off % PAGE_SIZE);
			uint32_t chunk = (uint32_t)(PAGE_SIZE - in);
			if (chunk > bytes)
				chunk = (uint32_t)bytes;
			uint8_t *src = drm_gem_page_virt(o, page);
			if (!src)
				return -EIO;
			kmemcpy(dst, src + in, chunk);
			dst += chunk;
			src_off += chunk;
			bytes -= chunk;
		}
	}
	vmsvga2_update_rect((uint32_t)x1, (uint32_t)y1, (uint32_t)(x2 - x1),
			    (uint32_t)(y2 - y1));
	return 0;
}

static int vmw_mode_set(struct drm_device *dev, struct drm_crtc *crtc,
			const struct drm_mode_modeinfo *mode,
			struct drm_framebuffer *fb, int x, int y)
{
	struct vmw_device *v = dev->priv;
	(void)crtc;
	(void)x;
	(void)y;

	/* Will a screen object or a screen target carry this mode?  Then its
	 * geometry comes from the command that defines it, and the mode
	 * registers below have no say in it -- and must not be given a veto,
	 * because the limits the device enforces on them are derived from the
	 * framebuffer aperture that this scan-out never reads. */
	int by_screen = vmw_stdu_available(v) ||
			(v->has_screen_object &&
			 (fb_is_surface(fb) ? v->has_3d : fb_is_gmr_scanout(fb)));

	/* On a device driven through command buffers the mode registers are
	 * not merely redundant beside a screen target -- programming them
	 * cycles SVGA_REG_ENABLE, and disabling the device resets it, which
	 * stops the command-buffer contexts: the very next screen-target
	 * command comes back SVGA_CB_STATUS_CB_HEADER_ERROR.  Upstream never
	 * touches these registers again after bring-up on such a device, and
	 * neither does this.  (cb_submit_raw() can restart a stopped context
	 * now, but not resetting the device at all is strictly better than
	 * recovering from it.)  The aperture path cannot need them either:
	 * it is only reached when the screen paths are refused, and it
	 * refuses a geometry the registers do not already carry. */
	if (!(by_screen && v->cb_ready) &&
	    (v->hw.width != mode->hdisplay || v->hw.height != mode->vdisplay ||
	     v->hw.bpp != 32)) {
		/* Where the console scans out of a buffer object of its own,
		 * this mode belongs to the client that asked for it and the
		 * console keeps its framebuffer and its geometry; it gets its
		 * own mode back when that client drops the display. */
		int rc = drm_console_active(dev) ?
				 vmsvga2_hw_set_mode_device(mode->hdisplay,
							    mode->vdisplay) :
				 vmsvga2_hw_set_mode(mode->hdisplay,
						     mode->vdisplay);
		if (rc != 0 && !by_screen)
			return -EINVAL;
	}
	vmsvga2_hw_geometry(&v->hw);
	/* Screen targets first: on a device with guest-backed objects this is
	 * the path the host expects, and the two below may be refused. */
	if (vmw_stdu_available(v)) {
		if (vmw_stdu_set_mode(v, mode->hdisplay, mode->vdisplay, fb) == 0) {
			v->screen_defined = 0;
			return 0;
		}
		/* Not fatal: fall through to the older paths, which is what a
		 * device that reports screen targets but refuses to define one
		 * leaves as the only way to show anything. */
		vmw_stdu_teardown(v);
	}
	/* Screen objects carry a surface only on a device WITHOUT guest-backed
	 * objects.  Where there are GB surfaces, BLIT_SURFACE_TO_SCREEN takes
	 * the command and shows nothing -- a silent success that reads as a
	 * black screen -- so the screen target above is the only path, and if
	 * it could not carry the mode the caller must hear about it. */
	if (v->has_screen_object && fb_is_surface(fb) && v->has_3d && !v->has_gb) {
		if (vmw_define_screen(v, mode->hdisplay, mode->vdisplay, fb->obj, 0) == 0)
			return vmw_surface_blit_screen(v, fb->obj, 0, 0,
						       mode->hdisplay, mode->vdisplay);
	}
	if (v->has_screen_object && fb_is_gmr_scanout(fb)) {
		if (vmw_define_screen(v, mode->hdisplay, mode->vdisplay, fb->obj,
				      fb->pitch) == 0)
			return vmw_sou_blit(v, fb->obj, fb->pitch, 0, 0,
					    mode->hdisplay, mode->vdisplay);
	}
	if (fb_is_surface(fb))
		return -ENODEV; /* no way to show a surface without screen objects */
	v->screen_defined = 0;
	/* The last path copies into the framebuffer aperture, which is still
	 * whatever geometry the mode registers accepted.  If they would not
	 * take this mode and no screen carried it either, copying anyway
	 * would put the top-left corner of the image on the screen and call
	 * it success.  Refuse instead, and the caller keeps the display it
	 * already had. */
	if (v->hw.width != mode->hdisplay || v->hw.height != mode->vdisplay)
		return -EINVAL;
	/* Legacy: the host reads VRAM; disable its own dirty tracking of
	 * VRAM writes, the copies below announce themselves. */
	vmsvga2_set_traces(0);
	return vmw_ldu_copy(v, fb, 0, 0, (int)fb->width, (int)fb->height);
}

static int vmw_crtc_disable(struct drm_device *dev, struct drm_crtc *crtc)
{
	struct vmw_device *v = dev->priv;
	(void)crtc;
	if (v->st_defined)
		vmw_stdu_teardown(v);
	v->screen_defined = 0;
	return 0;
}

static int vmw_fb_dirty(struct drm_device *dev, struct drm_crtc *crtc,
			struct drm_framebuffer *fb,
			const struct drm_mode_rect_k *rects, uint32_t n)
{
	struct vmw_device *v = dev->priv;
	int queued = 0;
	(void)crtc;

	/* A guest-backed surface is only displayable through a screen target;
	 * if the mode set did not leave one up, bring it up now. */
	if (!v->st_defined && fb && fb_is_surface(fb) && vmw_stdu_available(v))
		vmw_stdu_ensure(v, fb->width, fb->height);

	for (uint32_t i = 0; i < n; i++) {
		int rc;
		if (v->st_defined)
			rc = vmw_stdu_present(v, fb, rects[i].x1, rects[i].y1,
					      rects[i].x2, rects[i].y2, 0);
		else if (v->screen_defined && fb_is_surface(fb) && !v->has_gb)
			rc = vmw_surface_blit_screen(v, fb->obj, rects[i].x1, rects[i].y1,
						     rects[i].x2, rects[i].y2);
		else if (v->screen_defined && !fb_is_surface(fb) && fb->obj->priv &&
			 ((struct vmw_bo *)fb->obj->priv)->gmr_id == (int)v->scan_gmr) {
			/* Queued, not announced: a window drag arrives here as
			 * a run of rectangles, and ringing the doorbell for
			 * each one traps out of the virtual machine that many
			 * times.  One ring covers the whole run. */
			rc = vmw_sou_blit_ex(v, fb->obj, fb->pitch, rects[i].x1,
					     rects[i].y1, rects[i].x2,
					     rects[i].y2, 0);
			if (rc == 0)
				queued = 1;
		} else
			rc = vmw_ldu_copy(v, fb, rects[i].x1, rects[i].y1,
					  rects[i].x2, rects[i].y2);
		if (rc) {
			/* Report it, but never hand it back.  The X
			 * modesetting driver answers ONE failed DIRTYFB by
			 * unregistering its damage tracking for the rest of
			 * the session ("Disabling kernel dirty updates, not
			 * required." -- X_INFO, easy to miss), after which
			 * nothing asks for an update again and the screen is
			 * frozen whatever the driver does.  A rectangle that
			 * could not be shown is worth far less than the next
			 * one that could. */
			static int budget = 12;
			static int last_rc;
			if (rc != last_rc && budget > 0) {
				budget--;
				last_rc = rc;
				kprintf("[drm] vmwgfx: dirty rectangle %d,%d-%d,%d not shown (%d)\n",
					rects[i].x1, rects[i].y1, rects[i].x2,
					rects[i].y2, rc);
			}
		}
	}
	if (queued) {
		/* Announce the run.  With the command-buffer channel up the
		 * blits were gathered rather than written to the FIFO, so
		 * handing the gathered batch over IS the announcement. */
		if (v->cb_ready)
			vmw_cmd_flush(v);
		else
			vmsvga2_hw_doorbell();
	}
	return 0;
}

static int vmw_page_flip(struct drm_device *dev, struct drm_crtc *crtc,
			 struct drm_framebuffer *fb)
{
	struct vmw_device *v = dev->priv;

	if (!v->st_defined && fb && fb_is_surface(fb) && vmw_stdu_available(v))
		vmw_stdu_ensure(v, crtc->mode.hdisplay, crtc->mode.vdisplay);
	if (v->st_defined)
		return vmw_stdu_present(v, fb, 0, 0, (int)crtc->mode.hdisplay,
					(int)crtc->mode.vdisplay, 1);
	if (v->screen_defined && fb_is_surface(fb) && !v->has_gb) {
		if (v->scan_gmr != SVGA_GMR_NULL)
			vmw_define_screen(v, crtc->mode.hdisplay, crtc->mode.vdisplay,
					  fb->obj, 0);
		return vmw_surface_blit_screen(v, fb->obj, 0, 0,
					       crtc->mode.hdisplay, crtc->mode.vdisplay);
	}
	if (fb_is_surface(fb))
		return -ENODEV;
	if (v->screen_defined && fb->obj->priv &&
	    ((struct vmw_bo *)fb->obj->priv)->gmr_id >= 0 && fb->offset == 0) {
		/* Re-point the screen at the new buffer and show it. */
		if (vmw_define_screen(v, crtc->mode.hdisplay, crtc->mode.vdisplay,
				      fb->obj, fb->pitch) == 0)
			return vmw_sou_blit(v, fb->obj, fb->pitch, 0, 0,
					    crtc->mode.hdisplay, crtc->mode.vdisplay);
	}
	return vmw_ldu_copy(v, fb, 0, 0, (int)fb->width, (int)fb->height);
}

static int vmw_cursor_set(struct drm_device *dev, struct drm_crtc *crtc,
			  struct drm_gem_object *o, uint32_t w, uint32_t h,
			  int32_t hot_x, int32_t hot_y)
{
	(void)dev;
	(void)crtc;
	if (!o) {
		vmsvga2_cursor_show(0);
		return 0;
	}
	if (w > 64 || h > 64 || !o->pages)
		return -EINVAL;
	/* The object is ARGB at w*4 pitch; gather it into one buffer. */
	static uint32_t argb[64 * 64];
	for (uint32_t y = 0; y < h; y++) {
		uint64_t off = (uint64_t)y * w * 4;
		uint8_t *src = drm_gem_page_virt(o, (uint32_t)(off / PAGE_SIZE));
		if (!src)
			return -EIO;
		uint32_t in = (uint32_t)(off % PAGE_SIZE);
		uint32_t bytes = w * 4;
		uint8_t *dst = (uint8_t *)&argb[y * w];
		while (bytes) {
			uint32_t chunk = (uint32_t)(PAGE_SIZE - in);
			if (chunk > bytes)
				chunk = bytes;
			kmemcpy(dst, src + in, chunk);
			bytes -= chunk;
			dst += chunk;
			off += chunk;
			src = drm_gem_page_virt(o, (uint32_t)(off / PAGE_SIZE));
			in = 0;
			if (!src && bytes)
				return -EIO;
		}
	}
	if (vmsvga2_cursor_define_alpha(w, h, (uint32_t)(hot_x < 0 ? 0 : hot_x),
					(uint32_t)(hot_y < 0 ? 0 : hot_y), argb) != 0)
		return -ENODEV;
	vmsvga2_cursor_show(1);
	return 0;
}

static int vmw_cursor_move(struct drm_device *dev, struct drm_crtc *crtc, int x,
			   int y)
{
	(void)dev;
	(void)crtc;
	return vmsvga2_cursor_move(x, y, 1) == 0 ? 0 : -ENODEV;
}

static int vmw_dpms(struct drm_device *dev, struct drm_connector *c, int mode)
{
	(void)dev;
	(void)c;
	vmsvga2_display_enable(mode == 0);
	return 0;
}

static void vmw_fence_events_release(struct drm_file *fp);

static void vmw_postclose(struct drm_device *dev, struct drm_file *fp)
{
	/* Anything this file asked to be told about is dropped here: the
	 * event would be queued onto a file that no longer exists. */
	vmw_fence_events_release(fp);
	vmw_file_release(dev->priv, fp);
}

static void vmw_master_set(struct drm_device *dev, struct drm_file *fp)
{
	(void)dev;
	(void)fp;
	/* The console stops painting here rather than at the first mode set.
	 * It is a client of this device itself now (drm_console.c), and its
	 * own mode set is a mode set like any other -- taking the display
	 * away there would silence the console on behalf of the console. */
	vmsvga2_hw_display_take();
}

static void vmw_master_drop(struct drm_device *dev, struct drm_file *fp)
{
	struct vmw_device *v = dev->priv;
	(void)fp;
	v->screen_defined = 0;
	vmsvga2_cursor_show(0);
	vmsvga2_hw_display_release();
	vmsvga2_hw_geometry(&v->hw);
}

/* ---- driver ioctls ----------------------------------------------------- */

static int vmw_render_allowed(unsigned nr)
{
	switch (nr) {
	case DRM_VMW_GET_PARAM:
	case DRM_VMW_ALLOC_BO:
	case DRM_VMW_UNREF_DMABUF:
	case DRM_VMW_CREATE_CONTEXT:
	case DRM_VMW_UNREF_CONTEXT:
	case DRM_VMW_CREATE_SURFACE:
	case DRM_VMW_UNREF_SURFACE:
	case DRM_VMW_REF_SURFACE:
	case DRM_VMW_EXECBUF:
	case DRM_VMW_GET_3D_CAP:
	case DRM_VMW_FENCE_WAIT:
	case DRM_VMW_FENCE_SIGNALED:
	case DRM_VMW_FENCE_UNREF:
	case DRM_VMW_CREATE_SHADER:
	case DRM_VMW_UNREF_SHADER:
	case DRM_VMW_GB_SURFACE_CREATE:
	case DRM_VMW_GB_SURFACE_REF:
	case DRM_VMW_SYNCCPU:
	case DRM_VMW_CREATE_EXTENDED_CONTEXT:
	case DRM_VMW_GB_SURFACE_CREATE_EXT:
	case DRM_VMW_GB_SURFACE_REF_EXT:
	case DRM_VMW_MSG:
	/* Waiting for a fence through an event rather than an ioctl is the
	 * same operation with a different way of being told, so a render
	 * node may do it too.  UPDATE_LAYOUT is not here: it changes what
	 * the display advertises, which belongs to whoever holds the
	 * display. */
	case DRM_VMW_FENCE_EVENT:
		return 1;
	default:
		return 0;
	}
}

static long vmw_ioctl_get_param(struct vmw_device *v, struct drm_vmw_getparam_arg *a)
{
	switch (a->param) {
	case DRM_VMW_PARAM_NUM_STREAMS:
	case DRM_VMW_PARAM_NUM_FREE_STREAMS:
		a->value = 0;
		return 0;
	case DRM_VMW_PARAM_3D:
		a->value = v->has_3d;
		return 0;
	case DRM_VMW_PARAM_HW_CAPS:
		a->value = v->hw.caps;
		return 0;
	case DRM_VMW_PARAM_FIFO_CAPS:
		a->value = v->hw.fifo_caps;
		return 0;
	case DRM_VMW_PARAM_MAX_FB_SIZE:
		a->value = v->hw.vram_size;
		return 0;
	case DRM_VMW_PARAM_FIFO_HW_VERSION:
		a->value = vmsvga2_hw_has_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED) ?
				   vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED) :
				   vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION);
		return 0;
	case DRM_VMW_PARAM_MAX_SURF_MEMORY:
		a->value = 0x30000000;
		return 0;
	case DRM_VMW_PARAM_3D_CAPS_SIZE:
		if (v->has_gb)
			a->value = (SVGA3D_DEVCAP_MAX + 1) * 4;
		else
			a->value = v->has_3d ? (SVGA_FIFO_3D_CAPS_LAST - SVGA_FIFO_3D_CAPS + 1) * 4 : 0;
		return 0;
	case DRM_VMW_PARAM_MAX_MOB_MEMORY:
		a->value = v->max_mob_memory;
		return 0;
	case DRM_VMW_PARAM_MAX_MOB_SIZE:
		a->value = v->max_mob_size;
		return 0;
	case DRM_VMW_PARAM_SCREEN_TARGET:
		/* What upstream answers: whether the screen target is the
		 * display unit in use, not whether a size register answered. */
		a->value = vmw_stdu_available(v);
		return 0;
	case DRM_VMW_PARAM_DX:
		a->value = v->has_dx;
		return 0;
	case DRM_VMW_PARAM_HW_CAPS2:
		a->value = v->cap2;
		return 0;
	case DRM_VMW_PARAM_SM4_1:
		a->value = v->has_sm41;
		return 0;
	case DRM_VMW_PARAM_SM5:
		a->value = v->has_sm5;
		return 0;
	case DRM_VMW_PARAM_GL43:
		a->value = v->has_gl43;
		return 0;
	case DRM_VMW_PARAM_DEVICE_ID:
		a->value = v->drm.pci ? v->drm.pci->device_id : 0x0405;
		return 0;
	case DRM_VMW_PARAM_USER_SRF:
		a->value = 0;
		return 0;
	default:
		return -EINVAL;
	}
}

/* ---- fence events ------------------------------------------------------
 *
 * DRM_VMW_FENCE_EVENT asks for a DRM event when a fence signals, so that a
 * client can wait in its own poll() loop on the device rather than blocking
 * in an ioctl.  The fences here have no callback list, so the request is
 * parked in a small table that the fence check walks -- which already runs
 * from the device's interrupt and from the poll timer, so the event is
 * delivered as soon as the sequence passes with nothing else to arrange.
 *
 * The table is small on purpose: one pending event per in-flight frame is
 * the shape of every user of this, and a client that asks for more than
 * this many at once is told so rather than being allowed to pin memory. */
#define VMW_MAX_FENCE_EVENTS 32

struct vmw_fence_event {
	struct drm_fence *fence; /* NULL: slot free */
	struct drm_file *fp;
	uint64_t user_data;
	int want_time;
};

static struct vmw_fence_event g_fence_events[VMW_MAX_FENCE_EVENTS];
/* Taken from an interrupt handler -- vmw_irq_cb() -> vmw_fence_check() ->
 * vmw_fence_events_check() -- so EVERY acquisition of it disables interrupts
 * first, process-context ones included.
 *
 * With the plain spin_lock() that stood here, the DRM_VMW_FENCE_WAIT ioctl
 * (which calls vmw_fence_check() itself, in process context with interrupts
 * on) held this lock while the SVGA interrupt arrived on the same processor,
 * and the handler then spun for that same lock -- against a holder that could
 * not make progress until the handler returned.  A hard hang of that CPU, at
 * 500 million spins per report, with the interrupted ioctl frame still on the
 * stack underneath the handler's.
 *
 * In an interrupt handler the save/restore costs nothing (IF is already
 * clear); the guarantee it buys in process context is the whole point. */
static spinlock_t g_fence_event_lock;

static void vmw_fence_event_deliver(struct vmw_fence_event *e)
{
	struct drm_vmw_event_fence ev;

	mm_memset(&ev, 0, sizeof(ev));
	ev.base.type = DRM_VMW_EVENT_FENCE_SIGNALED;
	ev.base.length = sizeof(ev);
	ev.user_data = e->user_data;
	if (e->want_time) {
		uint64_t ns = e->fence->signal_ns;
		ev.tv_sec = (uint32_t)(ns / 1000000000ULL);
		ev.tv_usec = (uint32_t)((ns % 1000000000ULL) / 1000ULL);
	}
	drm_event_queue(e->fp, &ev, sizeof(ev));
}

/* Called after every fence signal sweep. */
static void vmw_fence_events_check(void)
{
	for (int i = 0; i < VMW_MAX_FENCE_EVENTS; i++) {
		struct vmw_fence_event *e = &g_fence_events[i];
		struct drm_fence *f;
		struct vmw_fence_event local;
		uint64_t fl;

		spin_lock_irqsave(&g_fence_event_lock, &fl);
		f = e->fence;
		if (!f || !f->signaled) {
			spin_unlock_irqrestore(&g_fence_event_lock, fl);
			continue;
		}
		local = *e;
		e->fence = NULL;
		spin_unlock_irqrestore(&g_fence_event_lock, fl);

		vmw_fence_event_deliver(&local);
		drm_fence_put(local.fence);
	}
}

/* Drop anything this file was waiting for; called when it closes. */
static void vmw_fence_events_release(struct drm_file *fp)
{
	for (int i = 0; i < VMW_MAX_FENCE_EVENTS; i++) {
		struct drm_fence *f = NULL;
		uint64_t fl;

		spin_lock_irqsave(&g_fence_event_lock, &fl);
		if (g_fence_events[i].fence && g_fence_events[i].fp == fp) {
			f = g_fence_events[i].fence;
			g_fence_events[i].fence = NULL;
		}
		spin_unlock_irqrestore(&g_fence_event_lock, fl);
		if (f)
			drm_fence_put(f);
	}
}

static long vmw_ioctl_fence_event(struct vmw_device *v, struct drm_file *fp,
				  struct drm_vmw_fence_event_arg *a)
{
	struct drm_fence *f;

	if (a->handle)
		f = drm_fence_handle_lookup(fp, a->handle);
	else
		f = vmw_fence_emit(v, 0); /* a fence for what has been queued */
	if (!f)
		return -EINVAL;

	/* Already done: the event is still owed, so it is delivered at once
	 * rather than the caller being left waiting for a signal that has
	 * already happened. */
	if (f->signaled) {
		struct vmw_fence_event e = { f, fp, a->user_data,
					     (a->flags & DRM_VMW_FE_FLAG_REQ_TIME) != 0 };
		vmw_fence_event_deliver(&e);
		if (!a->handle)
			drm_fence_put(f);
		return 0;
	}

	if (a->handle)
		drm_fence_get(f); /* the table holds its own reference */

	uint64_t fl;

	spin_lock_irqsave(&g_fence_event_lock, &fl);
	int slot = -1;
	for (int i = 0; i < VMW_MAX_FENCE_EVENTS; i++)
		if (!g_fence_events[i].fence) {
			slot = i;
			break;
		}
	if (slot >= 0) {
		g_fence_events[slot].fence = f;
		g_fence_events[slot].fp = fp;
		g_fence_events[slot].user_data = a->user_data;
		g_fence_events[slot].want_time =
			(a->flags & DRM_VMW_FE_FLAG_REQ_TIME) != 0;
	}
	spin_unlock_irqrestore(&g_fence_event_lock, fl);
	if (slot < 0) {
		drm_fence_put(f);
		return -EBUSY;
	}
	return 0;
}

/* ---- host-driven layout ------------------------------------------------
 *
 * The hypervisor resizes its window and tells the guest what geometry it
 * would like; a client that owns the display (an X server, or the tool that
 * follows the window size) passes those rectangles here.  What the driver
 * does with them is make the first one the connector's PREFERRED mode, so
 * that the next time anything asks the connector what it wants -- which is
 * what a mode-setting client does on a hot-plug event -- it is told the
 * size the host is actually showing.
 *
 * One output for now: the device allows several screen targets and this
 * driver exposes one connector.  The loop reads them all so that a caller
 * passing more is not an error, and the extras are ignored rather than
 * silently changing the one head that exists. */
static long vmw_ioctl_update_layout(struct vmw_device *v,
				    struct drm_vmw_update_layout_arg *a)
{
	struct drm_vmw_rect r;

	if (a->num_outputs == 0 || !a->rects)
		return -EINVAL;
	if (drm_copy_from_user(&r, (const void *)(uintptr_t)a->rects, sizeof(r)) != 0)
		return -EFAULT;
	if (!r.w || !r.h || r.w > v->drm.max_width || r.h > v->drm.max_height)
		return -EINVAL;

	struct drm_connector *c = &v->drm.conn[0];
	struct drm_mode_modeinfo m;

	drm_mode_fill(&m, r.w, r.h, 60, 1);
	/* Preferred is a property of exactly one mode. */
	for (uint32_t i = 0; i < c->nmodes; i++)
		c->modes[i].type &= (uint32_t)~DRM_MODE_TYPE_PREFERRED;
	/* Replace an existing entry of the same size, or add one. */
	for (uint32_t i = 0; i < c->nmodes; i++)
		if (c->modes[i].hdisplay == r.w && c->modes[i].vdisplay == r.h) {
			c->modes[i] = m;
			return 0;
		}
	if (drm_connector_add_mode(&v->drm, 0, &m) != 0)
		return -ENOSPC;
	return 0;
}

static long vmw_ioctl(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		      unsigned dir, void *kb, unsigned size, int *handled)
{
	struct vmw_device *v = dev->priv;
	(void)dir;
	*handled = 1;


	switch (nr) {
	case DRM_VMW_GET_PARAM:
		return vmw_ioctl_get_param(v, kb);
	case DRM_VMW_ALLOC_BO: {
		union drm_vmw_alloc_bo_arg *a = kb;
		uint32_t req_size = a->req.size;
		if (req_size == 0 || req_size > (512u << 20))
			return -EINVAL;
		struct drm_gem_object *o = drm_gem_alloc(dev, DRM_GEM_BO, req_size);
		if (!o)
			return -ENOMEM;
		int rc = drm_gem_alloc_pages(o);
		if (rc == 0)
			rc = vmw_gem_init(o);
		if (rc) {
			drm_gem_put(o);
			return rc;
		}
		uint32_t h;
		rc = drm_gem_handle_create(fp, o, &h);
		if (rc) {
			drm_gem_put(o);
			return rc;
		}
		struct vmw_bo *b = o->priv;
		a->rep.handle = h;
		a->rep.map_handle = drm_gem_mmap_offset(o);
		a->rep.cur_gmr_id = b->gmr_id >= 0 ? (uint32_t)b->gmr_id : 0;
		a->rep.cur_gmr_offset = 0;
		drm_gem_put(o);
		return 0;
	}
	case DRM_VMW_UNREF_DMABUF: {
		struct drm_vmw_handle_close_arg *a = kb;
		return drm_gem_handle_delete(fp, a->handle);
	}
	case DRM_VMW_SYNCCPU: {
		struct drm_vmw_synccpu_arg *a = kb;
		struct drm_gem_object *o = drm_gem_lookup(fp, a->handle);
		if (!o)
			return -ENOENT;
		int rc = 0;
		if (a->op == drm_vmw_synccpu_grab && o->fence && !o->fence->signaled) {
			if (a->flags & drm_vmw_synccpu_dontblock)
				rc = -EBUSY;
			else
				rc = drm_fence_wait(o->fence, VMW_FENCE_TIMEOUT_NS);
		}
		drm_gem_put(o);
		return rc;
	}
	case DRM_VMW_FENCE_WAIT: {
		struct drm_vmw_fence_wait_arg *a = kb;
		struct drm_fence *f = drm_fence_handle_lookup(fp, a->handle);
		if (!f)
			return -EINVAL;
		uint64_t to = a->timeout_us ? a->timeout_us * 1000ULL : VMW_FENCE_TIMEOUT_NS;
		vmw_fence_check(v);
		int rc = drm_fence_wait(f, to);
		drm_fence_put(f);
		if (rc == 0 && (a->wait_options & DRM_VMW_WAIT_OPTION_UNREF))
			drm_fence_handle_delete(fp, a->handle);
		return rc;
	}
	case DRM_VMW_FENCE_SIGNALED: {
		struct drm_vmw_fence_signaled_arg *a = kb;
		struct drm_fence *f = drm_fence_handle_lookup(fp, a->handle);
		if (!f)
			return -EINVAL;
		vmw_fence_check(v);
		a->signaled = f->signaled;
		a->signaled_flags = f->signaled ? f->flags : 0;
		a->passed_seqno = dev->fence_passed;
		drm_fence_put(f);
		return 0;
	}
	case DRM_VMW_FENCE_UNREF: {
		struct drm_vmw_fence_arg *a = kb;
		return drm_fence_handle_delete(fp, a->handle);
	}
	case DRM_VMW_GET_3D_CAP: {
		struct drm_vmw_get_3d_cap_arg *a = kb;
		if (!v->has_3d)
			return -ENODEV;
		if (v->has_gb) {
			uint32_t bytes = (SVGA3D_DEVCAP_MAX + 1) * 4;
			if (bytes > a->max_size)
				bytes = a->max_size;
			if (!validate_user_ptr(a->buffer, bytes) ||
			    copy_to_user((void *)(uintptr_t)a->buffer, v->devcaps, bytes) != 0)
				return -EFAULT;
			return 0;
		}
		uint32_t n = SVGA_FIFO_3D_CAPS_LAST - SVGA_FIFO_3D_CAPS + 1;
		uint32_t bytes = n * 4;
		if (bytes > a->max_size)
			bytes = a->max_size;
		if (!validate_user_ptr(a->buffer, bytes))
			return -EFAULT;
		for (uint32_t i = 0; i < bytes / 4; i++) {
			uint32_t w = vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_CAPS + i);
			if (copy_to_user((void *)(uintptr_t)(a->buffer + i * 4), &w, 4) != 0)
				return -EFAULT;
		}
		return 0;
	}
	/* The legacy surface pair: what a host without guest-backed objects
	 * offers, and the only 3D path there. */
	case DRM_VMW_CREATE_SURFACE:
		return vmw_ioctl_create_surface(v, fp, kb);
	case DRM_VMW_REF_SURFACE:
		return vmw_ioctl_ref_surface(v, fp, kb);
	case DRM_VMW_GB_SURFACE_CREATE:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_create(v, fp, kb, 0);
	case DRM_VMW_GB_SURFACE_CREATE_EXT:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_create(v, fp, kb, 1);
	case DRM_VMW_GB_SURFACE_REF:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_ref(v, fp, kb, 0);
	case DRM_VMW_GB_SURFACE_REF_EXT:
		if (!v->has_gb)
			return -ENODEV;
		return vmw_ioctl_gb_surface_ref(v, fp, kb, 1);
	case DRM_VMW_UNREF_SURFACE:
		return vmw_ioctl_unref_surface(v, fp, kb);
	case DRM_VMW_CREATE_CONTEXT: {
		struct drm_vmw_context_arg *a = kb;
		if (!v->has_3d)
			return -ENODEV;
		return vmw_ioctl_create_context(v, fp, 0, &a->cid);
	}
	case DRM_VMW_CREATE_EXTENDED_CONTEXT: {
		union drm_vmw_extended_context_arg *a = kb;
		int dx = a->req == drm_vmw_context_dx;
		if (!v->has_3d || (dx && !v->has_dx))
			return -ENODEV;
		return vmw_ioctl_create_context(v, fp, dx, &a->rep.cid);
	}
	case DRM_VMW_UNREF_CONTEXT: {
		struct drm_vmw_context_arg *a = kb;
		return vmw_ioctl_unref_context(v, fp, (uint32_t)a->cid);
	}
	case DRM_VMW_CREATE_SHADER:
		return vmw_ioctl_create_shader(v, fp, kb);
	case DRM_VMW_UNREF_SHADER: {
		struct drm_vmw_shader_arg *a = kb;
		return vmw_ioctl_unref_shader(v, fp, a->handle);
	}
	case DRM_VMW_EXECBUF: {
		struct drm_vmw_execbuf_arg arg;
		mm_memset(&arg, 0, sizeof(arg));
		/* Version 1 callers pass 24 bytes: no context, no fence fd. */
		mm_memcpy(&arg, kb, size < sizeof(arg) ? size : sizeof(arg));
		if (size < sizeof(arg)) {
			arg.context_handle = SVGA3D_INVALID_ID;
			arg.imported_fence_fd = -1;
			arg.flags = 0;
		}
		return vmw_execbuf(v, fp, &arg);
	}
	case DRM_VMW_FENCE_EVENT:
		return vmw_ioctl_fence_event(v, fp, kb);
	case DRM_VMW_UPDATE_LAYOUT:
		return vmw_ioctl_update_layout(v, kb);
	case DRM_VMW_MSG:
		return vmw_ioctl_msg(v, kb);
	default:
		return v->has_3d ? -ENOSYS : -ENODEV;
	}
}

/* ---- probe ---------------------------------------------------------- */

static const struct drm_driver vmw_driver = {
	.name = "vmwgfx",
	.desc = "VMware SVGA II display-manager driver",
	.date = "20260827",
	.major = 2,
	.minor = 20,
	.patch = 0,
	.cursor_w = 64,
	.cursor_h = 64,
	.postclose = vmw_postclose,
	.master_set = vmw_master_set,
	.master_drop = vmw_master_drop,
	.gem_init = vmw_gem_init,
	.gem_free = vmw_gem_free,
	.gem_release_pages = vmw_gem_release_pages,
	.gem_page_phys = vmw_gem_page_phys,
	.gem_mmap_pte_extra = 0,
	.mode_set = vmw_mode_set,
	.crtc_disable = vmw_crtc_disable,
	.fb_dirty = vmw_fb_dirty,
	.page_flip = vmw_page_flip,
	.cursor_set = vmw_cursor_set,
	.cursor_move = vmw_cursor_move,
	.dpms = vmw_dpms,
	.hw_vblank = 0,
	.ioctl = vmw_ioctl,
	.render_allowed = vmw_render_allowed,
};

/* Does the host offer 3D?
 *
 * Two places carry the answer and which one is live depends on the device.
 *
 * A device with guest-backed objects answers through the device-capability
 * register: write the capability's index to SVGA_REG_DEV_CAP, read the
 * value back.  This is the only reliable source on such a device -- the
 * FIFO's own version fields are the older interface and the host is not
 * obliged to keep them filled in.
 *
 * An older device answers in the FIFO, in one of TWO slots.  The version
 * moved to SVGA_FIFO_3D_HWVERSION_REVISED (17) because the original slot
 * (7) sits inside the region a guest may write, so a guest that reset the
 * FIFO could destroy the value the host had put there; the host advertises
 * the move with SVGA_FIFO_CAP_3D_HWVERSION_REVISED.  Reading the original
 * slot on a device that has moved the field yields 0.
 *
 * Reading only slot 7 is what this did before, and this machine -- VMware
 * with 3D switched on, caps 0xfdffc3e2 (SVGA_CAP_3D and SVGA_CAP_GBOBJECTS
 * both set), fifo caps 0x77f (the REVISED bit set) -- reported "3d no",
 * with has_gb and has_dx switched off behind it and DRM_VMW_PARAM_3D
 * answering 0, so Mesa's svga driver would not bind and GL fell back to
 * llvmpipe.  DRM_VMW_PARAM_FIFO_HW_VERSION below already read the right
 * slot; this is the same knowledge applied where the decision is made. */
static int vmw_probe_3d(const struct vmw_device *v)
{
	if (!(v->hw.caps & SVGA_CAP_3D))
		return 0;
	if (v->hw.caps & SVGA_CAP_GBOBJECTS) {
		vmsvga2_hw_write_reg(SVGA_REG_DEV_CAP, SVGA3D_DEVCAP_3D);
		return vmsvga2_hw_read_reg(SVGA_REG_DEV_CAP) != 0;
	}
	if (!(v->hw.caps & SVGA_CAP_EXTENDED_FIFO))
		return 0;
	if (vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_3D_HWVERSION_REVISED) &&
	    vmsvga2_hw_has_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED))
		return vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION_REVISED) != 0;
	return vmsvga2_hw_fifo_reg(SVGA_FIFO_3D_HWVERSION) != 0;
}

/* ---- scan-out limits ---------------------------------------------------- */

/* The standard mode list, largest first.  Shared with the boot console
 * (kernel/dev/video/vmsvga2.c), so the console and a display server agree
 * about what this device can show. */
static const uint32_t vmw_builtin_modes[][2] = {
	{ 2560, 1600 }, { 2560, 1440 }, { 1920, 1440 }, { 1920, 1200 },
	{ 1920, 1080 }, { 1856, 1392 }, { 1792, 1344 }, { 1680, 1050 },
	{ 1600, 1200 }, { 1600, 900 },	{ 1440, 900 },	{ 1400, 1050 },
	{ 1366, 768 },	{ 1360, 768 },	{ 1280, 1024 }, { 1280, 960 },
	{ 1280, 800 },	{ 1280, 768 },	{ 1280, 720 },	{ 1152, 864 },
	{ 1024, 768 },	{ 800, 600 },	{ 640, 480 },
};

#define VMW_SCANOUT_MAX_DIM 8192U

/* The build's screen-size preference (Makefile: SCREEN_SIZE,
 * MAX_SCREEN_SIZE), the same ceilings the boot console applies.  They bound
 * the mode this comes up in, not the mode list: a device that can scan out
 * more than the build asked for still offers those modes to a display
 * server that explicitly asks for one. */
#ifndef SCREEN_MAX_WIDTH
#define SCREEN_MAX_WIDTH 0xFFFFFFFFU
#endif
#ifndef SCREEN_MAX_HEIGHT
#define SCREEN_MAX_HEIGHT 0xFFFFFFFFU
#endif
#if defined(SCREEN_LARGE)
#define VMW_PREF_MAX_WIDTH 1920U
#define VMW_PREF_MAX_HEIGHT 1200U
#else
#define VMW_PREF_MAX_WIDTH 1280U
#define VMW_PREF_MAX_HEIGHT 800U
#endif

/* How large a screen this device can really show -- which is not what the
 * framebuffer aperture suggests.
 *
 * The console this driver takes over from puts its pixels in the aperture,
 * so its mode is bounded by the graphics memory the virtual machine was
 * configured with, and the device derives SVGA_REG_MAX_WIDTH/MAX_HEIGHT from
 * exactly that: 4 MB of it comes back as 1176x885, which is a statement
 * about the aperture and not about any display.
 *
 * A screen object or a screen target never reads that aperture.  The image
 * stays in guest memory and the host is told where it is, so the bounds that
 * apply are the ones the reference driver uses on such a device: the primary
 * surface memory the device reports, the largest image it can sample, and --
 * for a screen object, which is scanned out of a guest memory region -- the
 * device's limit on how large such a region may be.  Under those the screen
 * can be far larger than the aperture would ever have allowed.
 *
 * Every widening is conditional on the device reporting the register that
 * justifies it; a device that reports nothing keeps the aperture's bounds,
 * which is what this driver used for every mode before.
 */
static void vmw_scanout_limits(struct vmw_device *v)
{
	uint64_t mem = v->hw.vram_size;
	uint32_t w = v->hw.max_width ? v->hw.max_width : VMW_SCANOUT_MAX_DIM;
	uint32_t h = v->hw.max_height ? v->hw.max_height : VMW_SCANOUT_MAX_DIM;

	v->scanout_in_guest_memory = v->has_screen_object || v->has_screentarget;
	if (v->scanout_in_guest_memory) {
		uint32_t reg;

		if (v->hw.caps & SVGA_CAP_GBOBJECTS) {
			reg = vmsvga2_hw_read_reg(SVGA_REG_MAX_PRIMARY_MEM);
			if ((uint64_t)reg > mem)
				mem = reg;
		}
		if (v->has_gb) {
			reg = v->devcaps[SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH];
			if (reg > w)
				w = reg;
			reg = v->devcaps[SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT];
			if (reg > h)
				h = reg;
		}
		if (v->has_gb) {
			/* Only a bound, and only where the device states one:
			 * a zero here means "unstated", not "zero-sized". */
			reg = vmsvga2_hw_read_reg(SVGA_REG_SCREENTARGET_MAX_WIDTH);
			if (reg && reg < w)
				w = reg;
			reg = vmsvga2_hw_read_reg(SVGA_REG_SCREENTARGET_MAX_HEIGHT);
			if (reg && reg < h)
				h = reg;
		} else if (v->hw.caps & SVGA_CAP_GMR2) {
			/* The register is only meaningful on a device with the
			 * second-generation regions; on an older one there is
			 * nothing to read and the bounds above stand. */
			reg = vmsvga2_hw_read_reg(SVGA_REG_GMRS_MAX_PAGES);
			if (reg && (uint64_t)reg * PAGE_SIZE < mem)
				mem = (uint64_t)reg * PAGE_SIZE;
		}
	}
	if (w > VMW_SCANOUT_MAX_DIM)
		w = VMW_SCANOUT_MAX_DIM;
	if (h > VMW_SCANOUT_MAX_DIM)
		h = VMW_SCANOUT_MAX_DIM;
	v->scanout_max_mem = mem;
	v->scanout_max_width = w;
	v->scanout_max_height = h;
}

static int vmw_mode_fits(const struct vmw_device *v, uint32_t w, uint32_t h)
{
	return w != 0 && h != 0 && w <= v->scanout_max_width &&
	       h <= v->scanout_max_height &&
	       (uint64_t)w * h * 4 <= v->scanout_max_mem;
}

/* ...and is one the build allows the machine to come up in.
 * MAX_SCREEN_SIZE overrules everyone, the host included. */
static int vmw_mode_allowed(const struct vmw_device *v, uint32_t w, uint32_t h)
{
	return vmw_mode_fits(v, w, h) && w <= SCREEN_MAX_WIDTH &&
	       h <= SCREEN_MAX_HEIGHT;
}

/* ...and is one the build would pick on its own.  SCREEN_SIZE bounds what
 * the mode list may offer; it does not overrule the host. */
static int vmw_mode_preferable(const struct vmw_device *v, uint32_t w,
			       uint32_t h)
{
	return vmw_mode_allowed(v, w, h) && w <= VMW_PREF_MAX_WIDTH &&
	       h <= VMW_PREF_MAX_HEIGHT;
}

void vmwgfx_init(void)
{
	struct vmw_device *v = &g_vmw;

	if (!vmsvga2_hw_present())
		return;
	mm_memset(v, 0, sizeof(*v));
	vmsvga2_hw_geometry(&v->hw);
	v->has_gmr = (v->hw.caps & (SVGA_CAP_GMR | SVGA_CAP_GMR2)) != 0;
	v->has_screen_object = v->has_gmr &&
			       vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_SCREEN_OBJECT_2);
	v->has_3d = vmw_probe_3d(v);
	hrtimer_init(&v->fence_poll, vmw_fence_poll_fire, v);

	/* Guest-backed objects and the DX (VGPU10) command set. */
	v->has_gb = v->has_3d && (v->hw.caps & SVGA_CAP_GBOBJECTS) != 0;
	v->has_cmdbuf = (v->hw.caps & SVGA_CAP_COMMAND_BUFFERS) != 0;
	v->has_dx = v->has_gb && v->has_cmdbuf && (v->hw.caps & SVGA_CAP_DX) != 0;
	if (v->hw.caps & SVGA_CAP_CAP2_REGISTER)
		v->cap2 = vmsvga2_hw_read_reg(SVGA_REG_CAP2);
	if (v->has_gb) {
		for (uint32_t i = 0; i <= SVGA3D_DEVCAP_MAX; i++) {
			vmsvga2_hw_write_reg(SVGA_REG_DEV_CAP, i);
			v->devcaps[i] = vmsvga2_hw_read_reg(SVGA_REG_DEV_CAP);
		}
		/* Which register carries the guest-backed memory pool is
		 * decided by a capability, not by trying one and seeing
		 * whether it answers: on a device without CAP2_GB_MEMSIZE_2
		 * the 64-bit-capable register is simply not implemented and
		 * reads back whatever the register file has there -- a
		 * plausible-looking wrong number, not a zero the fallback
		 * would catch.
		 *
		 * The value matters well beyond bookkeeping: the client's GL
		 * stack flushes preemptively once a batch has referenced
		 * half of it, so reporting a pool far smaller than the real
		 * one turns every frame into a stream of tiny submissions. */
		if (v->cap2 & SVGA_CAP2_GB_MEMSIZE_2)
			v->max_mob_memory = (uint64_t)vmsvga2_hw_read_reg(
						    SVGA_REG_GBOBJECT_MEM_SIZE_KB) *
					    1024ULL;
		else
			v->max_mob_memory = (uint64_t)vmsvga2_hw_read_reg(
						    SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB) *
					    1024ULL;
		if (!v->max_mob_memory)
			v->max_mob_memory = 512ULL << 20;
		v->max_mob_size = vmsvga2_hw_read_reg(SVGA_REG_MOB_MAX_SIZE);
		if (!v->max_mob_size)
			v->max_mob_size = 128ULL << 20;
		v->has_screentarget = vmsvga2_hw_read_reg(SVGA_REG_SCREENTARGET_MAX_WIDTH) != 0;
		/* The message channel is not part of the device at all -- it
		 * is a port-I/O protocol the hypervisor answers -- so it is
		 * probed separately and its absence changes nothing else. */
		v->has_msg = vmw_msg_probe();
		if (v->has_dx) {
			v->has_dx = v->devcaps[SVGA3D_DEVCAP_DXCONTEXT] != 0;
			/* A shader model takes both halves of the answer: the
			 * device-capability register says the host can run
			 * it, and CAP2 says this device speaks the command
			 * set that comes with it -- SM4.1 rides on CAP2_DX2,
			 * SM5 on CAP2_DX3.  The devcap alone is not enough,
			 * and believing it means sending a command the device
			 * does not have, which halts the command-buffer
			 * context. */
			v->has_sm41 = v->has_dx && (v->cap2 & SVGA_CAP2_DX2) &&
				      v->devcaps[SVGA3D_DEVCAP_SM41] != 0;
			v->has_sm5 = v->has_sm41 && (v->cap2 & SVGA_CAP2_DX3) &&
				     v->devcaps[SVGA3D_DEVCAP_SM5] != 0;
			v->has_gl43 = v->has_sm5 && v->devcaps[SVGA3D_DEVCAP_GL43] != 0;
		}
		/* Report only what the validator carries. */
		if (!v->has_dx) {
			v->devcaps[SVGA3D_DEVCAP_DXCONTEXT] = 0;
			v->devcaps[SVGA3D_DEVCAP_SM41] = 0;
			v->devcaps[SVGA3D_DEVCAP_SM5] = 0;
			v->devcaps[SVGA3D_DEVCAP_GL43] = 0;
		}
	}
	vmw_scanout_limits(v);
	v->drm.max_width = v->scanout_max_width;
	v->drm.max_height = v->scanout_max_height;
	v->drm.refresh_hz = 60;
	if (v->has_gb)
		kprintf("[drm] vmwgfx: guest-backed memory pool %uMB, largest object %uMB\n",
			(uint32_t)(v->max_mob_memory >> 20),
			(uint32_t)(v->max_mob_size >> 20));
	kprintf("[drm] vmwgfx: scan-out from %s, up to %ux%u within %uKB\n",
		v->scanout_in_guest_memory ? "guest memory" :
					     "the framebuffer aperture",
		v->scanout_max_width, v->scanout_max_height,
		(uint32_t)(v->scanout_max_mem / 1024));
	if (drm_dev_register(&v->drm, &vmw_driver, vmsvga2_hw_pci(), v) != 0)
		return;

	/* Guest-backed objects come AFTER the device is registered: the object
	 * tables are GEM objects on this same drm_device, and the GEM layer
	 * reaches the backend through dev->drv, which registration is what
	 * fills in.  Setting them up first faulted on a NULL dev->drv the
	 * moment anything failed.
	 *
	 * Doing it here, at init, is what lets the console come up on KMS
	 * immediately below: the display moves to the guest-backed model once
	 * and the console is a client of it from the start, rather than the
	 * scan-out changing under a console that is already painting. */
	if (vmw_gb_init(v) != 0) {
		kprintf("[drm] vmwgfx: guest-backed object setup failed; 3D off\n");
		v->has_gb = 0;
		v->has_dx = 0;
		v->has_3d = 0;
	}
	/* vmw_cmdbuf_init() turns DX off if the command-buffer context would
	 * not start; the capability array has to say the same thing, because
	 * that is where Mesa reads DXCONTEXT from. */
	if (!v->has_dx) {
		v->devcaps[SVGA3D_DEVCAP_DXCONTEXT] = 0;
		v->devcaps[SVGA3D_DEVCAP_SM41] = 0;
		v->devcaps[SVGA3D_DEVCAP_SM5] = 0;
		v->devcaps[SVGA3D_DEVCAP_GL43] = 0;
		v->has_sm41 = 0;
		v->has_sm5 = 0;
		v->has_gl43 = 0;
	}

	/* One virtual connector.  The mode list is the standard set bounded by
	 * what this device can actually scan out, largest first, and the
	 * largest is the preferred one -- the console takes the first mode on
	 * the list and a display server asks for the preferred one, so this
	 * is where the resolution the machine comes up in is decided.  The
	 * mode the boot console left behind is kept on the list too, so
	 * falling back to it needs no modeset the device could refuse. */
	uint32_t pw = 0, ph = 0;
	/* What the host recommends, if it said and this scan-out can carry
	 * it.  The console driver asked at probe; the answer applies here
	 * rather than there, because here is where the aperture's limits
	 * stop applying and the recommendation usually becomes reachable. */
	if (vmsvga2_get_host_preferred(&pw, &ph) != 0 ||
	    !vmw_mode_allowed(v, pw, ph))
		pw = 0;
	for (unsigned i = 0;
	     i < sizeof(vmw_builtin_modes) / sizeof(vmw_builtin_modes[0]) && !pw;
	     i++) {
		if (vmw_mode_preferable(v, vmw_builtin_modes[i][0],
					vmw_builtin_modes[i][1])) {
			pw = vmw_builtin_modes[i][0];
			ph = vmw_builtin_modes[i][1];
		}
	}
	if (!pw) {
		pw = v->hw.width;
		ph = v->hw.height;
	}
	/* A size in millimetres at 96 dpi for the preferred mode. */
	uint32_t mm_w = pw * 254 / 960, mm_h = ph * 254 / 960;
	int c = drm_connector_add(&v->drm, DRM_MODE_CONNECTOR_VIRTUAL, mm_w, mm_h);
	if (c >= 0) {
		struct drm_mode_modeinfo m;
		drm_mode_fill(&m, pw, ph, 60, 1);
		drm_connector_add_mode(&v->drm, c, &m);
		if (v->hw.width != pw || v->hw.height != ph) {
			drm_mode_fill(&m, v->hw.width, v->hw.height, 60, 0);
			drm_connector_add_mode(&v->drm, c, &m);
		}
		for (unsigned i = 0;
		     i < sizeof(vmw_builtin_modes) / sizeof(vmw_builtin_modes[0]);
		     i++) {
			uint32_t sw = vmw_builtin_modes[i][0];
			uint32_t sh = vmw_builtin_modes[i][1];
			if (sw == pw && sh == ph)
				continue;
			if (sw == v->hw.width && sh == v->hw.height)
				continue;
			if (!vmw_mode_fits(v, sw, sh))
				continue;
			drm_mode_fill(&m, sw, sh, 60, 0);
			drm_connector_add_mode(&v->drm, c, &m);
		}
	}
	vmsvga2_hw_set_irq_callback(vmw_irq_cb);

	/* Put the console on this device's KMS path.
	 *
	 * Here, and not earlier: it needs the connector and its mode list,
	 * which is what the block above builds.  From this point the console
	 * draws into a buffer object that gets scanned out the same way an X
	 * server's does, instead of into the framebuffer in VRAM that the
	 * device no longer reads once it is in guest-backed mode.
	 *
	 * A failure is not one: the console keeps whatever it was already
	 * drawing into, which is the boot framebuffer, and the machine looks
	 * exactly as it did before this driver existed. */
	if (drm_console_takeover(&v->drm) != 0)
		kprintf("[drm] vmwgfx: console stays on the framebuffer\n");
	/* What is on the screen -- which with guest-memory scan-out is not
	 * what the mode registers say.  Those keep describing the framebuffer
	 * aperture the display no longer comes from: on a machine whose
	 * graphics memory cannot hold the mode, they stay at whatever the
	 * boot console could fit while the screen object shows the real one,
	 * and printing them made this line contradict the console's own. */
	uint32_t sw = v->hw.width, sh = v->hw.height;
	if (v->st_defined) {
		sw = v->st_w;
		sh = v->st_h;
	} else if (v->screen_defined) {
		sw = v->screen_w;
		sh = v->screen_h;
	}
	kprintf("[drm] vmwgfx: %ux%u, %s scan-out, gmr %s, 3d %s, gb %s, dx %s (sm4.1 %s, sm5 %s), cmdbuf %s, irq %s\n",
		sw, sh,
		vmw_stdu_available(v)   ? "screen-target" :
		v->has_screen_object	? "screen-object" : "legacy",
		v->has_gmr ? "yes" : "no", v->has_3d ? "yes" : "no",
		v->has_gb ? "yes" : "no", v->has_dx ? "yes" : "no",
		v->has_sm41 ? "yes" : "no", v->has_sm5 ? "yes" : "no",
		v->has_cmdbuf ? "yes" : "no",
		v->hw.irq_enabled ? "yes" : "polled");
}
