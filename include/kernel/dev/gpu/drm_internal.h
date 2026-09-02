// LikeOS-64 -- display-manager core: what the core's own files share.
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

#endif
