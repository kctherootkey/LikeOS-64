// LikeOS -- vmwgfx: the device's 3D capabilities.
//
// What the 3D device can do is a table of capability words: on a device
// with guest-backed objects one word per SVGA3D_DEVCAP_* index, read
// through SVGA_REG_DEV_CAP; on an older device a FIFO region in the
// SVGA3dCapsRecord format.  The client reads the table with
// DRM_VMW_GET_3D_CAP -- a client that knows about guest-backed objects in
// the device's own format, an older one in the FIFO format, which a
// guest-backed device then has to be translated into.
//
// The table lives in vmw_device.devcaps[]; this header is included at the
// end of vmw_gb.h, after struct vmw_device.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2021 VMware, Inc., Palo Alto, CA., USA

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_DEVCAPS_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_DEVCAPS_H

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>

/* Read the capability table from the device (once guest-backed objects
 * are known to be in use: vmw_device.has_gb).  0 or -errno. */
int vmw_devcaps_create(struct vmw_device *v);
void vmw_devcaps_destroy(struct vmw_device *v);
/* Bytes DRM_VMW_GET_3D_CAP returns to a client that is (gb_aware) or is not
 * aware of guest-backed objects. */
uint32_t vmw_devcaps_size(const struct vmw_device *v, bool gb_aware);
/* Copy the table, in the format the client understands, into the kernel
 * buffer `dst' of dst_size bytes (at most vmw_devcaps_size()).  `dst' must
 * come zeroed: the older format ends with an empty record that is not
 * written here.  0 or -errno. */
int vmw_devcaps_copy(struct vmw_device *v, bool gb_aware, void *dst,
		     uint32_t dst_size);

static inline uint32_t vmw_devcap_get(struct vmw_device *v, uint32_t devcap)
{
	bool gb_objects = !!(v->hw.caps & SVGA_CAP_GBOBJECTS);

	if (gb_objects && devcap <= SVGA3D_DEVCAP_MAX)
		return v->devcaps[devcap];
	return 0;
}

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_DEVCAPS_H */
