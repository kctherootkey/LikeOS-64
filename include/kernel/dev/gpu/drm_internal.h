// LikeOS -- display-manager core: what the core's own files share.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPU_DRM_INTERNAL_H
#define KERNEL_DEV_GPU_DRM_INTERNAL_H

#include <kernel/dev/gpu/drm.h>

int drm_master_set(struct drm_device *dev, struct drm_file *fp);
void drm_master_drop(struct drm_device *dev, struct drm_file *fp);
void drm_event_queue(struct drm_file *fp, const void *data, uint32_t length);

/* drm_kms.c */
void drm_kms_init(struct drm_device *dev);
void drm_kms_file_release(struct drm_device *dev, struct drm_file *fp);
long drm_kms_ioctl(struct drm_device *dev, struct drm_file *fp, unsigned nr,
		   void *kb, unsigned size, int *handled);

/* Kernel-owned framebuffer and mode set, for the in-kernel console client
 * (drm_console.c).  Defined in drm_kms.c. */
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

/* Object lookups and blobs (drm_kms.c), shared with drm_atomic.c. */
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
int drm_atomic_legacy_gamma(struct drm_device *dev, struct drm_file *fp,
			    struct drm_crtc *crtc, const uint16_t *r,
			    const uint16_t *g, const uint16_t *b);
/* One property on one object, the way OBJ_SETPROPERTY asks. */
int drm_atomic_legacy_set_property(struct drm_device *dev, struct drm_file *fp,
				   uint32_t obj_type, uint32_t obj_id,
				   uint32_t prop, uint64_t val);
long drm_atomic_ioctl(struct drm_device *dev, struct drm_file *fp,
		      struct drm_mode_atomic *a);
/* Commit the committed state again with every active crtc treated as a
 * fresh mode set (after a resume). */
int drm_atomic_replay(struct drm_device *dev);

#endif
