// LikeOS -- display-manager core: the per-crtc vblank counter.
//
// One per crtc in struct drm_device (vbl[]).  The counter advances from a
// timer where the core counts, or from the driver's vblank interrupt
// (drm_vblank_tick / drm_crtc_handle_vblank) where the backend counts, with
// the timer standing in as a watchdog when the interrupts stop.  The
// lifecycle -- one lock, no timer cancellation -- is described and
// implemented in drm_vblank.c.
//
// On top of that sits the reference-counted interface a driver may opt
// into: drm_crtc_vblank_get/put keep the vblank interrupt on while somebody
// needs it (and switch it off a while after the last user, when the driver
// has enable_vblank/disable_vblank), drm_crtc_vblank_on/off/reset follow
// the crtc through mode sets, a hardware frame counter (get_vblank_counter +
// max_vblank_count) accounts for vblanks no interrupt reported, and a
// scanout-position query (get_scanout_position) dates each vblank
// independently of interrupt latency.  A driver that provides none of these
// sees the counter exactly as before.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from the DRM vblank code and Intel's code: MIT
// Portions by Rickard E. (Rik) Faith <faith@valinux.com> and Gareth Hughes <gareth@valinux.com>
// Portions Copyright 2016 Intel Corp.

#ifndef KERNEL_DEV_GPU_DRM_VBLANK_H
#define KERNEL_DEV_GPU_DRM_VBLANK_H

#include <kernel/uapi/types.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/dev/gpu/drm_modes.h>

struct drm_device;
struct drm_file;
struct wait_queue_head;

/* How long the vblank interrupt stays on after its last user (ms): 0 never
 * switches it off, a negative value switches it off at once. */
#define DRM_VBLANK_OFFDELAY_MS 5000
/* The largest uncertainty accepted in a scanout-position timestamp (us);
 * 0 turns precise timestamps off. */
#define DRM_TIMESTAMP_PRECISION_US 20
/* How often a counter / timestamp read is repeated to get a consistent
 * pair, and a scanout-position query to get a precise one. */
#define DRM_TIMESTAMP_MAXRETRIES 3

/* How a crtc's vblank interrupt is switched off once nobody needs it
 * (drm_crtc_vblank_on_config). */
struct drm_vblank_crtc_config {
	/* delay after the last drm_crtc_vblank_put (ms), see
	 * DRM_VBLANK_OFFDELAY_MS for 0 and negative */
	int offdelay_ms;
	/* the counter and timestamps stay exact across an off/on of the
	 * interrupt (a hardware counter plus precise timestamps), so the
	 * interrupt may go off at the vblank after the last put, and a
	 * WAIT_VBLANK query is answered from the cached values */
	bool disable_immediate;
};

/* vblank: one counter per crtc, driven by hrtimer or the backend */
struct drm_vblank_crtc {
	uint64_t count;
	uint64_t last_ns;
	uint64_t period_ns;
	hrtimer_t timer;
	int running;
	/* How many waiters need the counter to keep advancing.  The
	 * timer runs while the CRTC is active OR this is non-zero, so
	 * that turning a CRTC off cannot strand someone mid-wait.
	 * This is also the drm_crtc_vblank_get/put reference count; the
	 * one reference drm_crtc_vblank_off/reset holds while the crtc is
	 * off (inmodeset) does not keep the timer running. */
	int refs;
	/* Where the backend counts (hw_vblank): when its last vblank
	 * arrived, whether the timer is standing in for it (no
	 * vblank for several periods while someone waits), and how
	 * often that was said */
	uint64_t hw_last_ns;
	int soft;
	uint32_t soft_said;

	/* ---- the reference-counted interface ---------------------
	 * last_ns above is the timestamp of the vblank `count' names: the
	 * delivery time, or the end of the vblank derived from the scanout
	 * position where the driver can say where the scanout is; 0 when it
	 * is not known. */
	struct drm_device *dev;
	int pipe;
	/* dev, pipe, config and disable_timer filled in (once) */
	int setup;
	/* The vblank interrupt is on and the counter counts.  Only
	 * consulted for a crtc the driver manages (below). */
	int enabled;
	/* Off between drm_crtc_vblank_off/reset and drm_crtc_vblank_on: one
	 * reference is held so that drm_crtc_vblank_get cannot switch the
	 * interrupt back on. */
	int inmodeset;
	/* The driver calls drm_crtc_vblank_on/off/reset for this crtc, so
	 * `enabled' decides whether vblanks are counted.  A crtc whose
	 * driver never does counts every vblank it is told of. */
	int managed;
	/* The hardware frame counter at the last update, and whether it
	 * has been read at all (the first reading is a base, not a
	 * difference). */
	uint32_t last;
	int last_valid;
	/* Per-crtc counter width (0: the driver's max_vblank_count). */
	uint32_t max_vblank_count;
	/* From the mode being scanned out (drm_calc_timestamping_constants):
	 * frame (field, interlaced) and line duration, and the mode itself
	 * for the scanout-position timestamps. */
	int framedur_ns;
	int linedur_ns;
	struct drm_display_mode hwmode;
	struct drm_vblank_crtc_config config;
	/* Switches the interrupt off DRM_VBLANK_OFFDELAY_MS after the last
	 * put.  Never cancelled: a get in the meantime makes it find a
	 * user when it fires, and it does nothing. */
	hrtimer_t disable_timer;
};

/* An event a client asked for at a vblank: kept on the device until the
 * crtc's counter reaches `target', then sent to `fp' and freed.  Drivers
 * make one with drm_vblank_event_create and hand it to
 * drm_crtc_arm_vblank_event (at the next vblank) or
 * drm_crtc_send_vblank_event (now). */
struct drm_pending_vblank_event {
	struct drm_pending_vblank_event *next;
	struct drm_file *fp;
	int crtc;
	uint64_t target;
	uint64_t user_data;
	int is_seq; /* drm_event_crtc_sequence rather than vblank */
	int is_flip; /* DRM_EVENT_FLIP_COMPLETE */
	uint64_t queued_ns; /* when it was asked for */
	uint64_t due_ns; /* when its vblank should have come */
	int late_said; /* reported as overdue */
	/* a drm_crtc_vblank_get reference that is dropped when the event
	 * is sent */
	int holds_ref;
};

/* Has counter value `seq' reached `ref'?  Within 2^23 vblanks behind
 * counts as reached; anything further is taken as a value ahead. */
static inline bool drm_vblank_passed(uint64_t seq, uint64_t ref)
{
	return (seq - ref) <= (1ULL << 23);
}

/* References: while one is held the counter counts (the driver's vblank
 * interrupt on, the core's timer running).  get fails with -EINVAL while
 * the crtc is off (drm_crtc_vblank_off) or the driver cannot switch the
 * interrupt on.  put never sleeps; callable from interrupt context. */
int drm_crtc_vblank_get(struct drm_device *dev, int crtc);
void drm_crtc_vblank_put(struct drm_device *dev, int crtc);
/* Wait (at most a second) for the next vblank; process context. */
int drm_crtc_wait_one_vblank(struct drm_device *dev, int crtc);

/* The crtc is being switched on / off: off answers every pending event
 * with the last count, drops the vblank works still waiting and refuses
 * references until on; it waits for a vblank work in progress, so it is
 * called in process context.  reset is off for a crtc that has never been
 * on (driver load, resume).  The first of these calls hands the crtc's
 * vblank handling to the driver: from then on vblanks are counted only
 * between on and off.  The driver's vblank hooks are called with the vblank
 * lock held and interrupts off. */
void drm_crtc_vblank_on(struct drm_device *dev, int crtc);
void drm_crtc_vblank_on_config(struct drm_device *dev, int crtc,
			       const struct drm_vblank_crtc_config *config);
void drm_crtc_vblank_off(struct drm_device *dev, int crtc);
void drm_crtc_vblank_reset(struct drm_device *dev, int crtc);
/* A per-crtc hardware counter width (call while the crtc is off). */
void drm_crtc_set_max_vblank_count(struct drm_device *dev, int crtc,
				   uint32_t max_vblank_count);
/* After the hardware lost its frame counter (a power state) while the
 * interrupt stayed logically on: rebase it from the time elapsed. */
void drm_crtc_vblank_restore(struct drm_device *dev, int crtc);

/* The counter, and the counter with the time of the vblank it names.
 * accurate_ fetches what the hardware counted since the last interrupt. */
uint64_t drm_crtc_vblank_count(struct drm_device *dev, int crtc);
uint64_t drm_crtc_vblank_count_and_time(struct drm_device *dev, int crtc,
					uint64_t *vblanktime_ns);
uint64_t drm_crtc_accurate_vblank_count(struct drm_device *dev, int crtc);
/* When the next vblank starts (needs precise timestamps); 0 or -EINVAL. */
int drm_crtc_next_vblank_start(struct drm_device *dev, int crtc,
			       uint64_t *vblanktime_ns);
/* Where waiters of this crtc's counter sleep. */
struct wait_queue_head *drm_crtc_vblank_waitqueue(struct drm_device *dev,
						  int crtc);

/* A vblank from the hardware, from the driver's interrupt handler.  False
 * when the crtc's vblank handling is off (and nothing was counted).
 * drm_vblank_tick (drm.h) is the same without the answer. */
bool drm_crtc_handle_vblank(struct drm_device *dev, int crtc);

/* Frame and line duration from the mode the crtc scans out, for the
 * time-based vblank accounting and the scanout-position timestamps.  A
 * driver calls it whenever the crtc's timing changes. _umode takes the
 * client form of the mode. */
void drm_calc_timestamping_constants(struct drm_device *dev, int crtc,
				     const struct drm_display_mode *mode);
void drm_calc_timestamping_constants_umode(struct drm_device *dev, int crtc,
					   const struct drm_mode_modeinfo *umode);

/* The end of the last vblank from the scanout position: what a driver with
 * get_scanout_position gets by default.  max_error in/out (ns). */
typedef bool (*drm_vblank_get_scanout_position_func)(
	struct drm_device *dev, int crtc, bool in_vblank_irq, int *vpos,
	int *hpos, uint64_t *stime_ns, uint64_t *etime_ns,
	const struct drm_display_mode *mode);
bool drm_crtc_vblank_helper_get_vblank_timestamp_internal(
	struct drm_device *dev, int crtc, int *max_error,
	uint64_t *vblank_time_ns, bool in_vblank_irq,
	drm_vblank_get_scanout_position_func get_scanout_position);
bool drm_crtc_vblank_helper_get_vblank_timestamp(struct drm_device *dev,
						 int crtc, int *max_error,
						 uint64_t *vblank_time_ns,
						 bool in_vblank_irq);

/* Events.  create: type is DRM_EVENT_VBLANK, DRM_EVENT_FLIP_COMPLETE or
 * DRM_EVENT_CRTC_SEQUENCE; NULL without memory.  send: out now, with the
 * current count and the time of its vblank (frees the event).  arm: out at
 * the next vblank; the caller's drm_crtc_vblank_get reference goes with
 * the event and is dropped when it is sent. */
struct drm_pending_vblank_event *drm_vblank_event_create(struct drm_file *fp,
							 uint32_t type,
							 uint64_t user_data);
void drm_crtc_send_vblank_event(struct drm_device *dev, int crtc,
				struct drm_pending_vblank_event *e);
void drm_crtc_arm_vblank_event(struct drm_device *dev, int crtc,
			       struct drm_pending_vblank_event *e);

#endif
