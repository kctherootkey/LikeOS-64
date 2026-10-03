// LikeOS -- vmwgfx: suspend and resume.
//
// The driver's drm_driver.suspend / .resume hooks.  The DRM core calls them
// with its power gate closed (no ioctl runs, no client can submit), around
// the replay of the committed display state that follows a resume.  Two
// ways down, chosen at build time by VMW_PM_RESET:
//
//   light (0): the device keeps running underneath, as across a sleep the
//     hypervisor preserves it through: the screens are taken down, the
//     command stream emptied and every fence waited for, and the hardware
//     layer quiesced without a reset.  Should the device nevertheless come
//     back without its state, resume rebuilds it as below.
//   reset (1): the device is handed back reset.  Its objects are rebuilt on
//     resume from what the driver keeps: object tables, every buffer's MOB
//     and region, the command-buffer channel.  Refused (-EBUSY, before
//     anything is touched) while a client holds a surface, a context or a
//     shader: their contents live on the device and cannot be saved.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_PM_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_PM_H

#include <kernel/dev/gpu/drm.h>

/* 1: the driver table carries vmw_pm_suspend / vmw_pm_resume (and the DRM
 * core offers the device's power_state file).  0: no power management, as
 * before. */
#ifndef VMW_PM
#define VMW_PM 1
#endif

/* 0: light suspend (the device is left running, nothing is reset).
 * 1: the device is reset on suspend and its objects rebuilt on resume;
 * refused while 3D resources are active. */
#ifndef VMW_PM_RESET
#define VMW_PM_RESET 0
#endif

/* 1: the cursor code takes the cursor down and brings it back itself
 * (vmw_cursor_suspend / vmw_cursor_resume, vmw_cursor.c), including the
 * cursor MOB a reset device no longer names.  0: suspend only hides the
 * hardware cursor, and the cursor comes back with the display replay that
 * follows a resume. */
#ifndef VMW_PM_CURSOR_HOOKS
#define VMW_PM_CURSOR_HOOKS 1
#endif

struct vmw_device;

/* drm_driver.suspend / .resume.  0 or -errno; a failed suspend leaves the
 * device running and its displays shown again, a failed resume leaves it
 * suspended (and may be retried).  Process context. */
int vmw_pm_suspend(struct drm_device *dev);
int vmw_pm_resume(struct drm_device *dev);

/* Tell the device which guest driver it talks to (vmw_drv.c): at init, and
 * again whenever the device comes back reset. */
void vmw_write_driver_id(const struct vmw_device *v,
			 const struct drm_driver *drv);

#if VMW_PM_CURSOR_HOOKS
/* vmw_cursor.c.  suspend: hide the cursor, keeping its image and position.
 * resume: `reinit' says the device lost its state -- the cursor MOB it was
 * shown from is defined again by then, but the device no longer names it
 * (SVGA_REG_CURSOR_MOBID): show the current image and position again, or
 * forget what is shown so that the display replay sets it again. */
void vmw_cursor_suspend(struct vmw_device *v);
void vmw_cursor_resume(struct vmw_device *v, int reinit);
#endif

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_PM_H */
