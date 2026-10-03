// LikeOS -- vmwgfx: display.
//
// Three ways onto the screen, chosen by what the host offers:
//   - screen targets (vmw_stdu.c): a guest-backed surface bound as the
//     scan-out, updated with UPDATE_GB_SCREENTARGET;
//   - screen objects (vmw_scrn.c): the image in a guest memory region,
//     carried onto the screen with BLIT_GMRFB_TO_SCREEN or, for a host
//     surface, BLIT_SURFACE_TO_SCREEN;
//   - legacy (vmw_ldu.c): the host scans out the framebuffer aperture and
//     the driver copies dirty rectangles into it.
// The DRM core drives them in one of two ways, chosen by the driver table
// (VMW_ATOMIC below): through the per-call hooks of struct drm_driver
// (mode_set, fb_dirty, page_flip, ...), which vmw_kms.c takes to the display
// unit of the crtc each one names, or through checked atomic requests
// (vmw_kms_atomic.c), which apply a whole new display state -- units off,
// modes, page flips, damage, cursors -- through the same per-unit code.
// Dirty rectangles (DIRTYFB, the console's pushes) reach fb_dirty either way.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2011-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_KMS_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_KMS_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/gpu_util.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>

struct vmw_device;
struct vmw_surface;

/* How the DRM core drives the display.
 *
 * 1: atomic mode setting.  The driver table carries atomic_check /
 * atomic_commit (vmw_atomic_check(), vmw_atomic_commit()) and the feature
 * bits that come with them -- FB_DAMAGE_CLIPS, IN_FENCE_FD / OUT_FENCE_PTR,
 * the cursor HOTSPOT_X / HOTSPOT_Y properties -- and the core turns every
 * legacy call (SETCRTC, PAGE_FLIP, SETPLANE, CURSOR, DPMS, the console's mode
 * sets) into an atomic request.  DPMS off switches the unit off and keeps
 * its planes; DPMS on sets the mode again.
 * 0: the per-call hooks (vmw_mode_set(), vmw_page_flip(), vmw_cursor_set(),
 * vmw_dpms(), ...), as before; DPMS blanks the device and leaves the units
 * as they are.
 *
 * Both sets of entry points are always built; this picks the driver table
 * (vmw_drv.c). */
#ifndef VMW_ATOMIC
#define VMW_ATOMIC 1
#endif

/* With atomic mode setting: DPMS off on the first unit also blanks the
 * device (vmsvga2_display_enable(0)), DPMS on unblanks it before the mode is
 * set again.  Switching a unit off does not blank everything by itself: the
 * primary screen object is only forgotten, never destroyed (vmw_scrn.c),
 * and the legacy display keeps showing the aperture, so without this a
 * screen saver's DPMS off would leave the last picture up on those paths.
 * 0: DPMS off switches the unit off and nothing else. */
#ifndef VMW_ATOMIC_DPMS_BLANK
#define VMW_ATOMIC_DPMS_BLANK 1
#endif

/* Which display path carries the screen. */
enum vmw_display_unit_type {
	vmw_du_invalid = 0,
	vmw_du_legacy,
	vmw_du_screen_object,
	vmw_du_screen_target,
	vmw_du_max
};

/* The path in use: screen targets where vmw_stdu_available(), screen
 * objects where the host has them, the aperture otherwise. */
enum vmw_display_unit_type vmw_active_display_unit(struct vmw_device *v);

/* ---- drm_driver hooks (vmw_kms.c) ---- */
int vmw_mode_set(struct drm_device *dev, struct drm_crtc *crtc,
		 const struct drm_mode_modeinfo *mode,
		 struct drm_framebuffer *fb, int x, int y);
int vmw_crtc_disable(struct drm_device *dev, struct drm_crtc *crtc);
int vmw_display_verify(struct drm_device *dev);
void vmw_display_fallback(struct drm_device *dev);
int vmw_fb_dirty(struct drm_device *dev, struct drm_crtc *crtc,
		 struct drm_framebuffer *fb,
		 const struct drm_mode_rect_k *rects, uint32_t n);
int vmw_page_flip(struct drm_device *dev, struct drm_crtc *crtc,
		  struct drm_framebuffer *fb);
int vmw_dpms(struct drm_device *dev, struct drm_connector *c, int mode);
void vmw_master_set(struct drm_device *dev, struct drm_file *fp);
void vmw_master_drop(struct drm_device *dev, struct drm_file *fp);

/* DRM_VMW_UPDATE_LAYOUT */
long vmw_ioctl_update_layout(struct vmw_device *v,
			     struct drm_vmw_update_layout_arg *a);

/* ---- atomic mode setting (vmw_kms_atomic.c) ----
 *
 * drm_driver.atomic_check: refuses what the display units cannot show --
 * a crtc without a unit, a further unit without screen targets or screen
 * objects, a mode beyond the scan-out's limits, a framebuffer the unit's
 * path cannot carry, a cursor image the device cannot take, a layout
 * beyond the device's display memory.
 * drm_driver.atomic_commit: applies a checked state in the order the device
 * needs: video overlays paused if any unit changes its mode, units that go
 * off switched off, modes set, page flips and damage shown, cursors placed,
 * overlays resumed.  What cannot be shown after the modes are set is
 * reported, never returned: a client that has one update refused may stop
 * sending them.  A mode set that fails is returned when it was the only
 * one in the request, as the legacy call would have returned it.
 * Commits are serialised by the display lock (vmw_kms_lock()). */
int vmw_atomic_check(struct drm_device *dev, struct drm_atomic_state *st);
int vmw_atomic_commit(struct drm_device *dev, struct drm_atomic_state *st);

/* Is the device driven by atomic requests (the driver table has
 * atomic_commit)? */
int vmw_kms_is_atomic(struct vmw_device *v);
/* The display lock: atomic commits and the host layout's writes to the
 * units' pref_* / gui_* fields.  Sleeping; taken before the overlay and
 * cursor locks, never while holding them.  fb_dirty and the console's
 * pushes do not take it. */
void vmw_kms_lock(struct vmw_device *v);
void vmw_kms_unlock(struct vmw_device *v);
/* Can the device show this desktop -- `n' screens at the given places and
 * sizes -- at all?  Each screen within the largest screen target, all of
 * them within the primary memory and, unless screen targets are free of
 * it, their bounding box as well.  Any number of screens.  0 or -EINVAL. */
int vmw_kms_check_display_memory(struct vmw_device *v, uint32_t n,
				 const struct drm_vmw_rect *rects);

/* ---- client-driven presents (DRM_VMW_PRESENT / _READBACK) ----
 *
 * vmw_kms_present(): show `clips' (num_clips rectangles, in surface
 * coordinates) of surface `srf' (device id `sid') at (dest_x, dest_y) of the
 * screen `fb' is on.  vmw_kms_readback(): copy `clips' of the screen back
 * into the buffer object behind `fb'; `user_fence_rep' is the client's
 * struct drm_vmw_fence_rep address, 0 for none.  Both dispatch on
 * vmw_active_display_unit(). */
int vmw_kms_present(struct vmw_device *v, struct drm_file *fp,
		    struct drm_framebuffer *fb, struct vmw_surface *srf,
		    uint32_t sid, int32_t dest_x, int32_t dest_y,
		    struct drm_vmw_rect *clips, uint32_t num_clips);
int vmw_kms_readback(struct vmw_device *v, struct drm_file *fp,
		     struct drm_framebuffer *fb, uint64_t user_fence_rep,
		     struct drm_vmw_rect *clips, uint32_t num_clips);

/* ---- display units (vmw_kms.c) ----
 *
 * One display unit per head: connector i, encoder i and crtc i of the DRM
 * device are unit i, and on a screen-target device unit i is screen target
 * i.  VMW_MAX_HEADS is how many units the driver drives.  With 1 the device
 * has the one connector vmwgfx_init() creates; with more,
 * vmw_kms_init_display() adds a connector per further unit, the host's
 * layout (DRM_VMW_UPDATE_LAYOUT) connects and places them, and every hook
 * dispatches on the unit of its crtc.  The legacy path is a single display
 * whatever this says: units beyond the first need screen objects or screen
 * targets. */
#ifndef VMW_MAX_HEADS
#define VMW_MAX_HEADS 1
#endif

/* "Every unit showing the framebuffer", where a unit number is asked for. */
#define VMW_DU_ALL_UNITS (-1)

struct vmw_display_unit {
	int unit;			/* its index: connector, crtc, screen id */
	/* What the host's layout asks of it (DRM_VMW_UPDATE_LAYOUT): whether
	 * it is part of the desktop, at what size, and where. */
	int pref_active;
	uint32_t pref_w, pref_h;
	int32_t gui_x, gui_y;
	/* What it shows: the mode, the framebuffer, and the framebuffer pixel
	 * at its top-left.  Set by a mode set before the scan-out path runs
	 * (which reads them), kept only when the mode set succeeds.  `active'
	 * is 0 once the unit is switched off and, with atomic mode setting,
	 * once the display has been handed back (vmw_master_drop()): the
	 * next commit that keeps its crtc on then sets the mode again rather
	 * than flipping onto a screen the device may no longer hold. */
	int active;
	uint32_t w, h;
	int32_t crtc_x, crtc_y;
	uint32_t fb_id;
	/* The screen object (vmw_scrn.c).  `sou_scan_gmr' is the guest
	 * memory region whose image is on the screen (SVGA_GMR_NULL for a
	 * surface); a screen with a backing store has it in the aperture, at
	 * `sou_backing_offset', `sou_backing_size' bytes (0: no backing
	 * store).  Unit 0's state is mirrored in vmw_device.screen_defined,
	 * screen_w, screen_h and scan_gmr. */
	int sou_defined;
	uint32_t sou_w, sou_h;
	int32_t sou_root_x, sou_root_y;
	uint32_t sou_scan_gmr;
	uint32_t sou_backing_offset, sou_backing_size;
};

/* Unit `unit' (its index filled in), NULL when out of range. */
struct vmw_display_unit *vmw_du(struct vmw_device *v, int unit);

/* What one unit does, for the legacy hooks and the atomic commit alike
 * (vmw_kms.c).
 *
 * vmw_du_mode_set(): show `fb' on the unit at `mode', from framebuffer
 * pixel (x, y), through the first path that carries it (screen target,
 * screen object, legacy display); the mode registers are programmed only
 * where no screen carries the mode or the device is not driven through
 * command buffers.  On failure the unit keeps the geometry it had.
 * vmw_du_disable(): the unit goes off (its screen target torn down, its
 * screen object destroyed or forgotten).
 * vmw_du_flip(): show a new framebuffer on the unit at its current mode
 * (w x h), without a mode set.
 * vmw_du_dirty(): show rectangles of `fb' (framebuffer coordinates) on the
 * unit; what cannot be shown is reported, never returned. */
int vmw_du_mode_set(struct vmw_device *v, struct vmw_display_unit *du,
		    const struct drm_mode_modeinfo *mode,
		    struct drm_framebuffer *fb, int x, int y);
void vmw_du_disable(struct vmw_device *v, struct vmw_display_unit *du);
int vmw_du_flip(struct vmw_device *v, struct vmw_display_unit *du,
		struct drm_framebuffer *fb, uint32_t w, uint32_t h);
void vmw_du_dirty(struct vmw_device *v, struct vmw_display_unit *du,
		  struct drm_framebuffer *fb,
		  const struct drm_mode_rect_k *rects, uint32_t n);
/* How many units there are: the device's connectors, at most
 * VMW_MAX_HEADS. */
int vmw_du_count(struct vmw_device *v);
/* After the first connector exists, before the console takes the display:
 * the units' initial state and, with VMW_MAX_HEADS above 1, a connector
 * for every further unit the display path can carry (disconnected until
 * the host's layout names it). */
int vmw_kms_init_display(struct vmw_device *v);
/* The size to offer when nothing better is known: the size the device came
 * up in (vmsvga2_get_initial_size()) where the scan-out can carry it, the
 * current mode-register geometry otherwise. */
void vmw_kms_initial_size(struct vmw_device *v, uint32_t *w, uint32_t *h);

/* A walk over clip rectangles and the units they land on.
 *
 * vmw_kms_helper_dirty() takes rectangles in framebuffer (or, with
 * dest_x/dest_y, source) coordinates -- `clips' as x1,y1-x2,y2 or `vclips'
 * as x,y,w,h, one of them NULL -- and for every unit concerned moves each
 * into unit coordinates, drops it when it misses the unit, clips it to the
 * unit and hands it to clip(), which adds it to the command in `cmd' and
 * bumps num_hits.  fifo_commit() sends what was built: once a command holds
 * `max_hits' rectangles, and once at the end of every unit (with num_hits
 * possibly 0).  `cmd' is the caller's buffer, big enough for max_hits.
 * `unit' is one unit, or VMW_DU_ALL_UNITS for every active unit showing
 * the framebuffer; `unit_ok', when set, further limits the walk to the
 * units it accepts.  The first failure a callback meets goes in `rc'. */
struct vmw_kms_dirty {
	void (*fifo_commit)(struct vmw_kms_dirty *);
	void (*clip)(struct vmw_kms_dirty *);
	int (*unit_ok)(const struct vmw_display_unit *du);
	struct vmw_device *v;
	struct vmw_display_unit *unit;
	void *cmd;
	u32 max_hits;
	u32 num_hits;
	s32 fb_x, fb_y;
	s32 unit_x1, unit_y1, unit_x2, unit_y2;
	int rc;
};
int vmw_kms_helper_dirty(struct vmw_device *v, struct drm_framebuffer *fb,
			 const struct drm_mode_rect_k *clips,
			 const struct drm_vmw_rect *vclips, s32 dest_x,
			 s32 dest_y, uint32_t num_clips, int increment,
			 int unit, struct vmw_kms_dirty *dirty);

/* ---- screen objects (vmw_scrn.c) ----
 *
 * vmw_sou_can_carry(): would a screen object take this framebuffer at all
 * (the device has them, and the framebuffer is a scan-out region or, with
 * 3D, a surface)?  vmw_sou_mode_set(): define unit `du' at the mode and
 * show `fb' on it; 1 when the screen object path does not carry this
 * framebuffer (or the device refused the screen) and the caller should try
 * the next path, else 0 or the error of the first blit.  vmw_sou_fb_dirty()
 * / vmw_sou_page_flip(): the same for an update; 1 when the unit's screen
 * object does not show this framebuffer.  vmw_sou_disable(): the crtc went
 * off.  vmw_sou_forget(): the device no longer holds the unit's screen (a
 * reset, the display handed back); nothing is sent. */
int vmw_sou_can_carry(struct vmw_device *v, struct drm_framebuffer *fb);
int vmw_sou_mode_set(struct vmw_device *v, struct vmw_display_unit *du,
		     const struct drm_mode_modeinfo *mode,
		     struct drm_framebuffer *fb);
int vmw_sou_fb_dirty(struct vmw_device *v, struct vmw_display_unit *du,
		     struct drm_framebuffer *fb,
		     const struct drm_mode_rect_k *rects, uint32_t n);
int vmw_sou_page_flip(struct vmw_device *v, struct vmw_display_unit *du,
		      struct drm_framebuffer *fb);
void vmw_sou_disable(struct vmw_device *v, struct vmw_display_unit *du);
void vmw_sou_forget(struct vmw_device *v, struct vmw_display_unit *du);
/* The screen-object halves of vmw_kms_present()/vmw_kms_readback(): show
 * `vclips' of surface `srf' at (dest_x, dest_y) of every screen showing
 * `fb', or copy `vclips' of those screens into `fb''s buffer object and
 * report a fence for it at `user_fence_rep'.  `unit' as for
 * vmw_kms_helper_dirty(); `out_fence', when not NULL, receives a fence for
 * the present (a reference, or NULL). */
int vmw_kms_sou_do_surface_dirty(struct vmw_device *v,
				 struct drm_framebuffer *fb,
				 const struct drm_mode_rect_k *clips,
				 const struct drm_vmw_rect *vclips,
				 struct vmw_surface *srf, s32 dest_x, s32 dest_y,
				 uint32_t num_clips, int inc,
				 struct drm_fence **out_fence, int unit);
int vmw_kms_sou_do_bo_dirty(struct vmw_device *v, struct drm_framebuffer *fb,
			    const struct drm_mode_rect_k *clips,
			    const struct drm_vmw_rect *vclips,
			    uint32_t num_clips, int increment,
			    struct drm_fence **out_fence, int unit);
int vmw_kms_sou_readback(struct vmw_device *v, struct drm_file *fp,
			 struct drm_framebuffer *fb, uint64_t user_fence_rep,
			 const struct drm_vmw_rect *vclips, uint32_t num_clips,
			 int unit);

/* ---- the legacy display (vmw_ldu.c) ----
 *
 * One display, unit 0: the host scans out the framebuffer aperture at the
 * mode the registers carry.  vmw_ldu_mode_set() shows `fb' at the current
 * register mode (-EINVAL when the registers do not carry this mode);
 * vmw_ldu_copy() copies a rectangle of `fb' (framebuffer coordinates) into
 * the aperture and announces it.  vmw_legacy_display_retire(): before
 * another path takes the screen -- see the definition. */
int vmw_ldu_mode_set(struct vmw_device *v, struct vmw_display_unit *du,
		     const struct drm_mode_modeinfo *mode,
		     struct drm_framebuffer *fb);
int vmw_ldu_copy(struct vmw_device *v, struct vmw_display_unit *du,
		 struct drm_framebuffer *fb, int x1, int y1, int x2, int y2);
void vmw_legacy_display_retire(struct vmw_device *v);

/* ---- screen-target scan-out (vmw_stdu.c) ----
 *
 * Each screen target is a display unit, numbered from 0 (the primary) up to
 * vmw_stdu_num_units() - 1; the unit number is the target id.  The
 * functions without a unit argument are unit 0, and the st_* fields of
 * struct vmw_device mirror unit 0 for readers outside vmw_stdu.c. */
struct drm_framebuffer;
/* Is this the device's scan-out path?  (Guest-backed objects up, and the
 * device advertises screen targets.) */
int vmw_stdu_available(struct vmw_device *v);
int vmw_stdu_set_mode(struct vmw_device *v, uint32_t w, uint32_t h,
		      struct drm_framebuffer *fb);
/* Bring a screen target of this size up if there is not one already. */
int vmw_stdu_ensure(struct vmw_device *v, uint32_t w, uint32_t h);
/* Formats a screen target can scan out. */
int vmw_format_is_screen_target(uint32_t format);
int vmw_stdu_present(struct vmw_device *v, struct drm_framebuffer *fb, int x1,
		     int y1, int x2, int y2, int full);
void vmw_stdu_teardown(struct vmw_device *v);
/* Surface id `sid' is being released: no screen target may go on believing
 * it shows that id.  See vmw_surface_destroy(). */
void vmw_stdu_forget_sid(struct vmw_device *v, uint32_t sid);

/* Per unit.  vmw_stdu_unit_set_mode(): define unit `unit' at w x h, placed
 * at (gui_x, gui_y) in the host's desktop, showing the framebuffer from its
 * pixel (crtc_x, crtc_y); then show `fb' on it (NULL: define only).
 * vmw_stdu_unit_present(): show the rectangle x1,y1-x2,y2 (framebuffer
 * coordinates) of `fb' on the unit; `full' is a mode set or page flip.
 * vmw_stdu_unit_defined(): 1 and the size when the unit's target is up.
 * Unit numbers out of range are -EINVAL (or "not defined"). */
int vmw_stdu_num_units(struct vmw_device *v);
int vmw_stdu_unit_set_mode(struct vmw_device *v, int unit, uint32_t w,
			   uint32_t h, int32_t gui_x, int32_t gui_y,
			   int32_t crtc_x, int32_t crtc_y,
			   struct drm_framebuffer *fb);
int vmw_stdu_unit_ensure(struct vmw_device *v, int unit, uint32_t w,
			 uint32_t h);
int vmw_stdu_unit_present(struct vmw_device *v, int unit,
			  struct drm_framebuffer *fb, int x1, int y1, int x2,
			  int y2, int full);
void vmw_stdu_unit_teardown(struct vmw_device *v, int unit);
int vmw_stdu_unit_defined(struct vmw_device *v, int unit, uint32_t *w,
			  uint32_t *h);

/* For the `unit' argument below: every unit showing the framebuffer. */
#define VMW_STDU_ALL_UNITS (-1)

/* The screen-target halves of vmw_kms_present()/vmw_kms_readback().
 * `unit' is a screen target, or VMW_STDU_ALL_UNITS; `inc' is the clip-array
 * stride in rectangles; `clips' is kernel memory.  `out_fence', when not
 * NULL, receives a fence for the update (a reference, or NULL). */
int vmw_kms_stdu_surface_dirty(struct vmw_device *v, struct drm_framebuffer *fb,
			       struct drm_vmw_rect *clips,
			       struct vmw_surface *srf, int32_t dest_x,
			       int32_t dest_y, uint32_t num_clips, int inc,
			       struct drm_fence **out_fence, int unit);
int vmw_kms_stdu_readback(struct vmw_device *v, struct drm_file *fp,
			  struct drm_framebuffer *fb, uint64_t user_fence_rep,
			  struct drm_vmw_rect *clips, uint32_t num_clips,
			  int inc, int unit);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_KMS_H */
