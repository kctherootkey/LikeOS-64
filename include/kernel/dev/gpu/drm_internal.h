// LikeOS -- display-manager core: what the core's own files share.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_DRM_INTERNAL_H
#define KERNEL_DEV_GPU_DRM_INTERNAL_H

#include <kernel/dev/gpu/drm.h>

/* ---- behaviour switches of the core ---------------------------------------
 *
 * Each is 1 for the behaviour described and 0 for what the core did before
 * it; both values build. */

/* A crtc is "enabled" while it has a mode and "active" while its pipe
 * runs.  DPMS off clears only active: the mode, the planes and the
 * connectors stay, and "a plane on a crtc that is off" is refused only for
 * a crtc without a mode.  An enabled crtc must have a connector and a
 * connector's crtc must be enabled.  0: a crtc without its pipe running
 * has no mode either, and DPMS off on an atomic driver is refused as long
 * as the primary plane shows a framebuffer. */
#ifndef DRM_ATOMIC_ENABLE_SEMANTICS
#define DRM_ATOMIC_ENABLE_SEMANTICS 1
#endif

/* The legacy cursor call on an atomic driver asks drv->cursor_obj_check,
 * or checks the image size against plain buffers only.  0: every object
 * must hold w * h * 4 bytes. */
#ifndef DRM_CURSOR_OBJ_CHECK
#define DRM_CURSOR_OBJ_CHECK 1
#endif

/* HOTSPOT_X / HOTSPOT_Y on cursor planes and the client capability that
 * makes cursor planes visible to atomic clients, for drivers with
 * DRM_FEATURE_CURSOR_HOTSPOT.  0: neither; the capability is accepted
 * from any client and changes nothing. */
#ifndef DRM_CURSOR_HOTSPOT
#define DRM_CURSOR_HOTSPOT 1
#endif

/* The legacy gamma table (crtc->gamma_size, 256) is separate from
 * GAMMA_LUT_SIZE, which each crtc may set to its own value (as may
 * DEGAMMA_LUT_SIZE).  0: drv->gamma_size is both, the same on every crtc. */
#ifndef DRM_COLOR_LEGACY_GAMMA_SPLIT
#define DRM_COLOR_LEGACY_GAMMA_SPLIT 1
#endif

/* CTM is attached to the crtcs of a DRM_FEATURE_COLOR_MGMT driver only
 * when drv->color_has_ctm says the hardware has one.  0: always. */
#ifndef DRM_COLOR_CTM_BY_DRIVER
#define DRM_COLOR_CTM_BY_DRIVER 1
#endif

/* Suspend and resume (and /sys power_state) for every driver with the two
 * hooks, atomic or not; a driver without atomic entry points is shown
 * again through its mode_set / cursor / dpms hooks.  0: atomic drivers
 * only. */
#ifndef DRM_PM_ANY_DRIVER
#define DRM_PM_ANY_DRIVER 1
#endif

/* The power gate: ioctls wait while the device is suspended, and a
 * suspend waits for the ioctls in flight (drm_pm_gate_enter).  0: no gate;
 * drm_pm_gate_* always let the caller through. */
#ifndef DRM_PM_GATE
#define DRM_PM_GATE 1
#endif

/* "link-status" on atomic drivers: a request may set a BAD link back to
 * GOOD (never GOOD to BAD: only the driver finds a link bad), the legacy
 * SETCRTC sets the links of the crtc's connectors GOOD, and a connector
 * whose link status the request changes makes its crtc's update a full
 * mode set, so that the link is trained again.  0: link-status writes are
 * accepted and change nothing. */
#ifndef DRM_LINK_STATUS_SEMANTICS
#define DRM_LINK_STATUS_SEMANTICS 1
#endif

/* A flip-complete event on a crtc whose driver switches its vblank
 * interrupt (enable_vblank) takes its vblank reference first and then
 * aims at the vblank after the count brought up to date by it: with the
 * interrupt off between users the count is stale until the reference
 * switches it on, and an event aimed from the stale count would go out at
 * once.  0: the target is the count as it stands plus one. */
#ifndef DRM_FLIP_EVENT_VBLANK_REF
#define DRM_FLIP_EVENT_VBLANK_REF 1
#endif

/* An OUT_FENCE_PTR fence of an atomic request holds a vblank reference
 * from the commit until it signals (on a driver with enable_vblank), so
 * that the counter it watches counts: the fence signals at the vblank, not
 * after the give-up time.  0: no reference. */
#ifndef DRM_OUT_FENCE_VBLANK_REF
#define DRM_OUT_FENCE_VBLANK_REF 1
#endif

int drm_master_set(struct drm_device *dev, struct drm_file *fp);
/* drm_pm_gate_enter() with the choice of being interruptible (1) or not
 * (0: a file's release, which cannot fail). */
int drm_pm_gate_enter_flags(struct drm_device *dev, int intr);
void drm_master_drop(struct drm_device *dev, struct drm_file *fp);
void drm_event_queue(struct drm_file *fp, const void *data, uint32_t length);

/* drm_kms.c */
void drm_kms_init(struct drm_device *dev);
void drm_kms_file_release(struct drm_device *dev, struct drm_file *fp);
long drm_kms_ioctl(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		   void *kb, unsigned size, int *handled);

/* Kernel-owned framebuffer and mode set, for the in-kernel console client
 * (drm_console.c).  Defined in drm_framebuffer.c and drm_crtc.c. */
int drm_kms_fb_add_kernel(struct drm_device *dev, struct drm_gem_object *o,
			  uint32_t w, uint32_t h, uint32_t pitch,
			  uint32_t *id_out);
int drm_kms_crtc_set_kernel(struct drm_device *dev, uint32_t crtc_index,
			    const struct drm_mode_modeinfo *mode,
			    uint32_t fb_id);
/* The same for any format (the cursor wrapper), owner NULL. */
int drm_kms_fb_add_internal(struct drm_device *dev, struct drm_gem_object *o,
			    uint32_t w, uint32_t h, uint32_t pitch,
			    uint32_t format, uint32_t *id_out);
void drm_kms_fb_remove_internal(struct drm_device *dev, uint32_t id);

/* Object lookups and blobs (drm_crtc.c, drm_connector.c, drm_plane.c,
 * drm_property.c), shared with drm_atomic.c. */
struct drm_crtc *drm_crtc_find(struct drm_device *dev, uint32_t id);
struct drm_connector *drm_conn_find(struct drm_device *dev, uint32_t id);
struct drm_plane *drm_plane_find(struct drm_device *dev, uint32_t id);
struct drm_blob *drm_blob_find(struct drm_device *dev, uint32_t id);
uint32_t drm_blob_create(struct drm_device *dev, const void *data,
			 uint32_t length);
void drm_blob_destroy(struct drm_device *dev, uint32_t id);
/* Keep the crtc's vblank counter running (or let it stop) after a change
 * of its active state. */
void drm_kms_vbl_sync(struct drm_device *dev, int crtc);
/* A DRM_EVENT_FLIP_COMPLETE for `fp' at the crtc's next vblank. */
int drm_kms_queue_flip_event(struct drm_device *dev, struct drm_file *fp,
			     int crtc, uint64_t user_data);

/* ---- what the mode-setting files share ---------------------------------
 *
 * The mode-setting core is split by object: drm_kms.c (init, file release,
 * ioctl routing), drm_mode_object.c, drm_property.c, drm_crtc.c,
 * drm_plane.c, drm_connector.c, drm_framebuffer.c, drm_vblank.c,
 * drm_dumb_buffers.c and drm_lease.c.  Each ioctl handler takes the kernel
 * copy of the argument and returns 0 or a negative errno; drm_kms_ioctl()
 * has already checked the caller's rights. */

/* drm_mode_object.c: the next mode object id.  Ids come from one counter in
 * creation order and are never reused -- clients cache them. */
uint32_t drm_mode_id_alloc(struct drm_device *dev);
/* Copy up to `cap' of the `n' ids to the user array at `uptr' (nothing when
 * either is 0). */
int drm_copy_ids(uint64_t uptr, uint32_t cap, const uint32_t *ids, uint32_t n);
int drm_mode_obj_get_properties_ioctl(struct drm_device *dev, void *kb,
				      struct drm_file *fp);
int drm_mode_obj_set_property_ioctl(struct drm_device *dev, void *kb,
				    struct drm_file *fp);

/* drm_property.c: a new property (NULL when the table is full), an enum
 * value on it, a property by id. */
struct drm_prop *drm_prop_add(struct drm_device *dev, const char *name,
			      uint32_t flags);
void drm_prop_enum(struct drm_prop *p, uint64_t value, const char *name);
struct drm_prop *drm_prop_find(struct drm_device *dev, uint32_t id);
int drm_mode_getproperty_ioctl(struct drm_device *dev, void *kb,
			       struct drm_file *fp);
int drm_mode_getblob_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_createblob_ioctl(struct drm_device *dev, void *kb,
			      struct drm_file *fp);
int drm_mode_destroyblob_ioctl(struct drm_device *dev, void *kb,
			       struct drm_file *fp);

/* drm_plane.c: the format lists a plane gets when it names none, the
 * IN_FORMATS blob for a format/modifier list, and the crtc a primary or
 * cursor plane belongs to (*is_cursor says which). */
extern const uint32_t drm_cursor_formats[1];
extern const uint64_t drm_linear_modifiers[1];
extern const uint32_t drm_default_formats[3];
uint32_t drm_in_formats_blob(struct drm_device *dev, const uint32_t *formats,
			     uint32_t nformats, const uint64_t *mods,
			     uint32_t nmods);
struct drm_crtc *drm_plane_crtc(struct drm_device *dev, uint32_t plane_id,
				int *is_cursor);
int drm_mode_getplane_res(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_getplane(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_setplane(struct drm_device *dev, void *kb, struct drm_file *fp);

/* drm_framebuffer.c: a client framebuffer from an ADDFB2 request, owned by
 * `fp'; whatever shows a framebuffer stops showing it; a framebuffer
 * removed (taken off screen first); a closing file's framebuffers. */
int drm_fb_create(struct drm_device *dev, struct drm_file *fp,
		  struct drm_mode_fb_cmd2 *r, uint32_t *id_out);
void drm_fb_unshow(struct drm_device *dev, uint32_t id);
int drm_fb_remove(struct drm_device *dev, uint32_t id);
void drm_framebuffer_file_release(struct drm_device *dev, struct drm_file *fp);
int drm_mode_addfb_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_addfb2_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_rmfb_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_getfb(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_getfb2_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_dirtyfb_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);
/* CLOSEFB -- the file lets go of a framebuffer without taking it off
 * the screen (drm_framebuffer.c). */
int drm_mode_closefb_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);

/* drm_connector.c: an encoder by id; the connector ioctls. */
struct drm_encoder *drm_enc_find(struct drm_device *dev, uint32_t id);
int drm_mode_getencoder(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_getconnector(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_connector_property_set_ioctl(struct drm_device *dev, void *kb,
				     struct drm_file *fp);

/* drm_crtc.c: the legacy mode set -- a NULL mode or fb_id 0 switches the
 * crtc off; `fp' NULL is the kernel. */
int drm_crtc_set(struct drm_device *dev, struct drm_file *fp,
		 struct drm_crtc *crtc, const struct drm_mode_modeinfo *mode,
		 uint32_t fb_id, int x, int y);
int drm_mode_getresources(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_getcrtc(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_mode_setcrtc(struct drm_device *dev, void *kb, struct drm_file *fp);
/* CURSOR and CURSOR2; `nr' is the ioctl number (0xBB: CURSOR2). */
int drm_mode_cursor_common(struct drm_device *dev, void *kb, struct drm_file *fp,
			   unsigned nr);
int drm_mode_gamma_get_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp);
int drm_mode_gamma_set_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp);
int drm_mode_page_flip_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp);

/* drm_vblank.c: the counter's timer callback (initialised once, at
 * drm_kms_init); an event for `fp' when the crtc's counter reaches
 * `target'; a closing file's pending events. */
void drm_vbl_timer_fire(hrtimer_t *t);
int drm_vbl_queue_event(struct drm_device *dev, struct drm_file *fp, int crtc,
			uint64_t target, uint64_t user_data, int is_seq,
			int is_flip);
void drm_vblank_file_release(struct drm_device *dev, struct drm_file *fp);
int drm_wait_vblank_ioctl(struct drm_device *dev, void *kb, struct drm_file *fp);
int drm_crtc_get_sequence_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp);
int drm_crtc_queue_sequence_ioctl(struct drm_device *dev, void *kb,
				  struct drm_file *fp);

/* drm_dumb_buffers.c */
int drm_mode_create_dumb_ioctl(struct drm_device *dev, void *kb,
			       struct drm_file *fp);
int drm_mode_mmap_dumb_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp);
int drm_mode_destroy_dumb_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp);

/* drm_lease.c: leasing is not supported (-EINVAL). */
int drm_mode_create_lease_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp);
int drm_mode_list_lessees_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp);
int drm_mode_get_lease_ioctl(struct drm_device *dev, void *kb,
			     struct drm_file *fp);
int drm_mode_revoke_lease_ioctl(struct drm_device *dev, void *kb,
				struct drm_file *fp);

/* drm_atomic.c: the legacy calls as atomic states (drivers with
 * atomic_commit), and the ATOMIC ioctl. */
int drm_atomic_legacy_crtc_set(struct drm_device *dev, struct drm_file *fp,
			       struct drm_crtc *crtc,
			       const struct drm_mode_modeinfo *mode,
			       uint32_t fb_id, int x, int y,
			       const uint32_t *conn_ids, uint32_t nconn);
int drm_atomic_legacy_page_flip(struct drm_device *dev, struct drm_file *fp,
				struct drm_crtc *crtc, struct drm_framebuffer *fb,
				int event, uint64_t user_data);
int drm_atomic_legacy_set_plane(struct drm_device *dev, struct drm_file *fp,
				struct drm_plane *plane, uint32_t crtc_id,
				uint32_t fb_id, int32_t crtc_x, int32_t crtc_y,
				uint32_t crtc_w, uint32_t crtc_h, uint32_t src_x,
				uint32_t src_y, uint32_t src_w, uint32_t src_h);
int drm_atomic_legacy_cursor(struct drm_device *dev, struct drm_file *fp,
			     struct drm_crtc *crtc, uint32_t flags,
			     struct drm_gem_object *o, uint32_t w, uint32_t h,
			     int32_t hot_x, int32_t hot_y, int x, int y);
int drm_atomic_legacy_dpms(struct drm_device *dev, struct drm_file *fp,
			   struct drm_connector *c, int mode);
/* SETGAMMA: `size' entries per channel (the crtc's legacy size, <= 256). */
int drm_atomic_legacy_gamma(struct drm_device *dev, struct drm_file *fp,
			    struct drm_crtc *crtc, const uint16_t *r,
			    const uint16_t *g, const uint16_t *b, uint32_t size);
/* One property on one object, the way OBJ_SETPROPERTY asks. */
int drm_atomic_legacy_set_property(struct drm_device *dev, struct drm_file *fp,
				   uint32_t obj_type, uint32_t obj_id,
				   uint32_t prop, uint64_t val);
long drm_atomic_ioctl(struct drm_device *dev, struct drm_file *fp,
		      struct drm_mode_atomic *a);
/* Commit the committed state again with every active crtc treated as a
 * fresh mode set (after a resume). */
int drm_atomic_replay(struct drm_device *dev);
/* The same for a driver without atomic entry points: every running crtc's
 * mode, framebuffer and position through mode_set, its cursor through
 * cursor_set / cursor_move, then each connector's DPMS level. */
int drm_legacy_replay(struct drm_device *dev);

#endif
