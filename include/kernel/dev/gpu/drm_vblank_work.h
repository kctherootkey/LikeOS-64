// LikeOS -- display-manager core: work that runs at a vblank.
//
// Some display programming has to happen after a particular vblank and may
// take too long, or need to sleep, for an interrupt handler.  A vblank work
// is a function the driver schedules for a vblank count; when the crtc's
// counter reaches it, the function runs on the core's vblank worker thread
// -- in process context, as soon after the vblank as the scheduler allows.
// While it waits for its vblank the work holds a vblank reference, so the
// counter keeps counting.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from the DRM vblank work code: MIT
// (the files those portions derive from carry no copyright line)

#ifndef KERNEL_DEV_GPU_DRM_VBLANK_WORK_H
#define KERNEL_DEV_GPU_DRM_VBLANK_WORK_H

#include <kernel/uapi/types.h>
#include <kernel/dev/gpu/gpu_util.h>

struct drm_device;
struct drm_vblank_work;

typedef void (*drm_vblank_work_func_t)(struct drm_vblank_work *work);

struct drm_vblank_work {
	/* what runs, and for which crtc */
	drm_vblank_work_func_t func;
	struct drm_device *dev;
	int pipe;
	/* the vblank count it runs at (drm_vblank_work_schedule) */
	uint64_t count;
	/* drm_vblank_work_cancel_sync calls in progress: no rescheduling
	 * until they are done */
	int cancelling;
	/* on the pending list, waiting for its vblank */
	struct list_head node;
	/* on the worker's run list, its vblank reached */
	struct list_head run_node;
};

/* Before first use, in process context (starts the worker thread the
 * first time). */
void drm_vblank_work_init(struct drm_vblank_work *work, struct drm_device *dev,
			  int crtc, drm_vblank_work_func_t func);
/* Run `work' once the counter reaches `count'.  A count already passed runs
 * at once, or with nextonmiss at the next vblank.  Rescheduling a pending
 * work moves it.  1 when scheduled, 0 when it already was for that count
 * (or is being cancelled, or the crtc is off), negative on error.  Any
 * context. */
int drm_vblank_work_schedule(struct drm_vblank_work *work, uint64_t count,
			     bool nextonmiss);
/* Take it back; waits for a run in progress.  True when it was pending or
 * queued.  Process context. */
bool drm_vblank_work_cancel_sync(struct drm_vblank_work *work);
/* Wait until it has run (its vblank, then the function).  Process
 * context. */
void drm_vblank_work_flush(struct drm_vblank_work *work);
/* Same for every work of the crtc. */
void drm_vblank_work_flush_all(struct drm_device *dev, int crtc);

/* The vblank core's side (drm_vblank.c). */
/* A vblank was counted: what waited for it goes to the worker.  Any
 * context; called without the vblank lock. */
void drm_handle_vblank_works(struct drm_device *dev, int crtc);
/* The crtc goes off: pending works are dropped with their references. */
void drm_vblank_cancel_pending_works(struct drm_device *dev, int crtc);
/* Wait for the crtc's queued and running works.  Process context. */
void drm_vblank_flush_worker(struct drm_device *dev, int crtc);

#endif
