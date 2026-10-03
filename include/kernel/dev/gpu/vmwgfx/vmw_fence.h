// LikeOS -- vmwgfx: fences, the fence manager and fence events.
//
// A fence is a sequence number the device passes after the work submitted
// before it.  Emitted down the command-buffer channel while that is up (it
// passes when the buffer carrying it completes) and into the FIFO otherwise
// (it passes when the FIFO's fence register says so).  The DRM core's
// drm_fence carries the waiting; the fence manager (vmw_fence.c) keeps the
// device's pending fences in sequence order, decides when they have passed
// -- from the device interrupt, from a 2 ms poll timer while fences are
// outstanding, and from every waiter -- arms the fence interrupts a pending
// fence needs, and runs the actions queued on a fence when it passes (the
// DRM events clients asked for with DRM_VMW_FENCE_EVENT, among them).
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's and Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright 2011-2012 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#ifndef KERNEL_DEV_GPU_VMWGFX_VMW_FENCE_H
#define KERNEL_DEV_GPU_VMWGFX_VMW_FENCE_H

#include <kernel/dev/gpu/drm.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>

/* How long an in-kernel wait that nobody bounded waits by default. */
#define VMW_FENCE_TIMEOUT_NS 2000000000ULL
/* How long teardown and the "user space lost its fence" sync wait. */
#define VMW_FENCE_WAIT_TIMEOUT_NS (5ULL * 1000000000ULL)

struct vmw_device;
/* The pending-fence list, its actions and the interrupt holds; private to
 * vmw_fence.c, reached through vmw_device.fman. */
struct vmw_fence_manager;

/* Set up the manager, the poll timer and the interrupt's view of the
 * device.  Called once from vmwgfx_init(), before the device is
 * registered.  Returns 0. */
int vmw_fence_manager_init(struct vmw_device *v);
/* Signal everything still pending (waiting a bounded time for each), run
 * its actions, release the interrupt holds and stop the timer. */
void vmw_fence_manager_takedown(struct vmw_device *v);
/* Fence emission on/off around a FIFO shutdown: down waits for (and, past
 * VMW_FENCE_WAIT_TIMEOUT_NS, force-signals) every pending fence; fences
 * emitted while down are not tracked.  Process context. */
void vmw_fence_fifo_down(struct vmw_device *v);
void vmw_fence_fifo_up(struct vmw_device *v);
/* Power management (vmw_pm.c).  suspend: vmw_fence_fifo_down(), then the
 * fence goal, the interrupt holds and the poll timer stopped.  resume:
 * signal everything up to `passed' (the last sequence issued before the
 * suspend; 0: nothing), let the timer run again and vmw_fence_fifo_up().
 * Process context. */
void vmw_fence_suspend(struct vmw_device *v);
void vmw_fence_resume(struct vmw_device *v, uint32_t passed);

/* Insert a fence after whatever was just submitted; returns it (a reference
 * the caller drops), a signalled fence on a device without fences, or NULL. */
struct drm_fence *vmw_fence_emit(struct vmw_device *v, uint32_t flags);
/* Signal every fence the device has passed, then run the actions that
 * became due.  Safe from the interrupt handler and the timer. */
void vmw_fence_check(struct vmw_device *v);
/* The pending list against what has been signalled: retire passed fences,
 * run their actions, move the fence goal.  Returns the last sequence known
 * to have passed (what DRM_VMW_FENCE_SIGNALED and fence_rep report as
 * passed_seqno).  Any context. */
uint32_t vmw_fences_update(struct vmw_device *v);
/* Has `f' signalled?  Asks the device first when it has not. */
bool vmw_fence_obj_signaled(struct vmw_device *v, struct drm_fence *f);
/* Wait for `f' up to `timeout_ns' with the fence interrupt armed.  0 when
 * signalled, -EBUSY when the time ran out, -ERESTARTSYS when interrupted
 * (`interruptible' only).  `lazy' is accepted for the ioctl's sake and has
 * no effect.  Process context, no spinlock held. */
int vmw_fence_obj_wait(struct vmw_device *v, struct drm_fence *f, bool lazy,
		       bool interruptible, uint64_t timeout_ns);
/* vmw_fence_check(), then an interruptible wait for `f' up to `timeout_ns':
 * 0, -ETIMEDOUT or -ERESTARTSYS.  Process context. */
int vmw_fence_wait(struct vmw_device *v, struct drm_fence *f,
		   uint64_t timeout_ns);

/* The device interrupt (registered with vmsvga2_hw_set_irq_callback()). */
void vmw_irq_cb(uint32_t status);
/* drm_driver.fence_poll */
void vmw_drm_fence_poll(struct drm_device *dev);

/* Queue a DRM event for `fp' to be sent when `f' signals -- at once when it
 * already has.  `event' (a struct drm_event header and its payload, at most
 * 64 bytes) is copied.  When `tv_sec_off' is not negative, the fence's
 * signal time is written into the event as a u32 seconds field at that
 * byte offset and a u32 microseconds field at `tv_usec_off'.  0, -EINVAL,
 * or -ENOMEM (no memory, or the file has too many events pending).  Any
 * context that may allocate. */
int vmw_event_fence_action_queue(struct vmw_device *v, struct drm_file *fp,
				 struct drm_fence *f, const void *event,
				 uint32_t length, int32_t tv_sec_off,
				 int32_t tv_usec_off);

/* Fill a struct drm_vmw_fence_rep at user address `user_rep' (0: none):
 * `ret' as error, and on success the handle, the sequence and the passed
 * sequence; mask 0 (see the comment in the function).  If the copy fails on
 * success, the handle is deleted and the fence waited for, since user space
 * can no longer do either.  Process context. */
void vmw_fence_copy_user_rep(struct vmw_device *v, struct drm_file *fp,
			     int ret, uint64_t user_rep, struct drm_fence *f,
			     uint32_t handle, int32_t out_fence_fd);

/* DRM_VMW_FENCE_WAIT, DRM_VMW_FENCE_SIGNALED, DRM_VMW_FENCE_UNREF */
long vmw_fence_obj_wait_ioctl(struct vmw_device *v, struct drm_file *fp,
			      struct drm_vmw_fence_wait_arg *a);
long vmw_fence_obj_signaled_ioctl(struct vmw_device *v, struct drm_file *fp,
				  struct drm_vmw_fence_signaled_arg *a);
long vmw_fence_obj_unref_ioctl(struct vmw_device *v, struct drm_file *fp,
			       struct drm_vmw_fence_arg *a);
/* DRM_VMW_FENCE_EVENT */
long vmw_ioctl_fence_event(struct vmw_device *v, struct drm_file *fp,
			   struct drm_vmw_fence_event_arg *a);
/* A file is closing: drop every action it queued (nothing is sent to it
 * afterwards). */
void vmw_fence_file_release(struct vmw_device *v, struct drm_file *fp);

#endif /* KERNEL_DEV_GPU_VMWGFX_VMW_FENCE_H */
