// LikeOS VMware SVGA II display driver
//
// Supports the SVGA II virtual display adapter exposed by VMware products,
// QEMU ("-vga vmware") and VirtualBox ("VMSVGA").  Every optional feature is
// strictly capability-gated so the driver degrades gracefully on hosts with
// reduced implementations; when the device is absent or bring-up fails the
// kernel keeps using the GOP framebuffer path unchanged.
//
// This file: probe and bring-up, mode setting, the boot console (screen
// updates through the fb flush hook), the /dev/fb0 backend and the
// host-preferred resolution.  The register file and the display-manager
// exports are in vmsvga2_hw.c, the FIFO and fences in vmsvga2_fifo.c, the
// interrupt in vmsvga2_irq.c, guest memory regions in vmsvga2_gmr.c and the
// hardware cursor in vmsvga2_cursor.c.
//
// See include/kernel/dev/video/vmsvga2.h for the architecture and locking
// model overview.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include "vmsvga2_priv.h"
#include <kernel/dev/video/fb.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/hal/vmware_backdoor.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h> /* errno values */

static const struct fbdev_backend vmsvga2_fbdev_backend;

// The console's mode sets, its blanking and its boot-time rollback used to
// write SVGA_REG_ENABLE 0 and then 1 around every mode change.  Disabling the
// device resets it: the command-buffer contexts stop and the guest-backed
// objects of a display manager on the same device are gone.  With this 0 a
// mode set only makes sure the device is enabled (SVGA_REG_ENABLE_ENABLE,
// written when it is not already), blanking hides the legacy framebuffer
// (SVGA_REG_ENABLE_HIDE) on devices that have such state to lose, and a
// failed boot takeover puts back the mode the device was found in.  1
// restores the old sequence everywhere.
#ifndef VMSVGA2_ENABLE_TOGGLE
#define VMSVGA2_ENABLE_TOGGLE 0
#endif

// Size the display manager falls back to when the host recommends nothing
// and the mode registers hold something smaller: what a virtual machine
// window can be expected to hold.
#define SVGA_MIN_INITIAL_WIDTH 1280U
#define SVGA_MIN_INITIAL_HEIGHT 800U

// ===========================================================================
// Sleeping mutex (the kernel has no generic sleeping mutex; modesetting is
// infrequent and may sleep, so a tiny park/wake mutex is built here on the
// established wait_channel/BLOCKED protocol used by tty_read)
// ===========================================================================

typedef struct {
	spinlock_t lock;
	task_t *owner; // NULL = free; sentinel (task_t*)1 pre-scheduler
	int waiters;
} svga_mutex_t;

#define SVGA_MUTEX_BOOT_OWNER ((task_t *)1)

static void svga_mutex_init(svga_mutex_t *m, const char *name)
{
	spinlock_init(&m->lock, name);
	m->owner = NULL;
	m->waiters = 0;
}

static void svga_mutex_lock(svga_mutex_t *m)
{
	task_t *cur = sched_current();
	uint64_t f;

	might_sleep();
	WARN_ON_ONCE(irqs_disabled() && cur); // sleeping lock in atomic context

	for (;;) {
		spin_lock_irqsave(&m->lock, &f);
		if (!m->owner) {
			m->owner = cur ? cur : SVGA_MUTEX_BOOT_OWNER;
			spin_unlock_irqrestore(&m->lock, f);
			return;
		}
		BUG_ON(cur && m->owner == cur); // recursive lock: deadlock
		if (!cur) {
			// Pre-scheduler there is exactly one context; a held
			// mutex here is a driver bug.
			spin_unlock_irqrestore(&m->lock, f);
			BUG();
		}
		// Park atomically wrt unlock: publish BLOCKED+channel while
		// still holding m->lock so a concurrent unlocker's
		// sched_wake_channel() can claim us (same protocol as
		// tty_read).
		m->waiters++;
		cur->wait_channel = (void *)m;
		cur->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&m->lock, f);
		sched_schedule();
		spin_lock_irqsave(&m->lock, &f);
		m->waiters--;
		spin_unlock_irqrestore(&m->lock, f);
	}
}

static void svga_mutex_unlock(svga_mutex_t *m)
{
	task_t *cur = sched_current();
	uint64_t f;
	int wake;

	spin_lock_irqsave(&m->lock, &f);
	WARN_ON_ONCE(m->owner != (cur ? cur : SVGA_MUTEX_BOOT_OWNER) &&
		     m->owner != SVGA_MUTEX_BOOT_OWNER);
	m->owner = NULL;
	wake = m->waiters;
	spin_unlock_irqrestore(&m->lock, f);
	if (wake)
		sched_wake_channel((void *)m);
}

// ===========================================================================
// Driver state (shared with the other vmsvga2 files through vmsvga2_priv.h)
// ===========================================================================

svga_state_t g_svga;
svga_devinfo_t g_svga_info;

volatile int g_vmsvga_initialized = 0;

static svga_mutex_t svga_modeset_mutex;

#if VMSVGA2_PM
// What a suspend took down and a resume puts back (see the power management
// section at the end of this file).  Written under svga_modeset_mutex.
#define SVGA_PM_RUNNING 0
#define SVGA_PM_LIGHT 1 // quiesced, the device left running
#define SVGA_PM_RESET 2 // quiesced, then reset
#define SVGA_PM_SHUTDOWN 3 // handed back for good (vmsvga2_shutdown())
static struct {
	int state; // SVGA_PM_*
	uint32_t enable; // SVGA_REG_ENABLE at suspend
	// SVGA_REG_TRACES as the resume writes it: as found at suspend,
	// updated by vmsvga2_set_traces() while suspended.
	volatile uint32_t traces;
	uint32_t fence; // the device's fence register after the drain
	int crtc_enabled; // the console's scan-out was not blanked
} g_svga_pm;
#endif


// ===========================================================================
// Updates
// ===========================================================================

// The console's screen updates run as the framebuffer flush hook, i.e.
// wherever anything printed -- with interrupts off, under other locks, in an
// interrupt handler, before the scheduler runs.  So they never wait for the
// FIFO: an update is queued only if the FIFO lock is free and the ring has
// room right now (svga_fifo_try_write_cmd()), and otherwise its rectangle is
// merged into this pending box.  The box goes out ahead of the next update
// (merged with it: SVGA_CMD_UPDATE only says "show what VRAM holds here
// now", so one box covering both says everything two would), and from the
// FIFO's release points: the producer that held the lock looks at the box
// after dropping it (svga_fifo_commit_irqrestore() and
// svga_fifo_abort_irqrestore()), and vmsvga2_fifo_flush() once the ring has
// drained.  Nothing is lost; at worst an update shows a little late.
//
// svga_dirty_lock guards only the box: nothing else is taken or called
// while it is held, so it nests inside anything.  Coordinates are clipped
// to the mode, x2/y2 exclusive.
static spinlock_t svga_dirty_lock = SPINLOCK_INIT("svga_dirty");
static struct {
	uint32_t x1, y1, x2, y2;
	volatile int valid;
} g_svga_dirty;

static void svga_dirty_merge(uint32_t x1, uint32_t y1, uint32_t x2,
			     uint32_t y2)
{
	uint64_t f;

	spin_lock_irqsave(&svga_dirty_lock, &f);
	if (!g_svga_dirty.valid) {
		g_svga_dirty.x1 = x1;
		g_svga_dirty.y1 = y1;
		g_svga_dirty.x2 = x2;
		g_svga_dirty.y2 = y2;
		g_svga_dirty.valid = 1;
	} else {
		if (x1 < g_svga_dirty.x1)
			g_svga_dirty.x1 = x1;
		if (y1 < g_svga_dirty.y1)
			g_svga_dirty.y1 = y1;
		if (x2 > g_svga_dirty.x2)
			g_svga_dirty.x2 = x2;
		if (y2 > g_svga_dirty.y2)
			g_svga_dirty.y2 = y2;
	}
	spin_unlock_irqrestore(&svga_dirty_lock, f);
}

// Take the box out (it is empty afterwards); 0 when there was none.
static int svga_dirty_take(uint32_t *x1, uint32_t *y1, uint32_t *x2,
			   uint32_t *y2)
{
	uint64_t f;
	int valid;

	if (!g_svga_dirty.valid)
		return 0; // racy peek; the decision is made under the lock
	spin_lock_irqsave(&svga_dirty_lock, &f);
	valid = g_svga_dirty.valid;
	*x1 = g_svga_dirty.x1;
	*y1 = g_svga_dirty.y1;
	*x2 = g_svga_dirty.x2;
	*y2 = g_svga_dirty.y2;
	g_svga_dirty.valid = 0;
	spin_unlock_irqrestore(&svga_dirty_lock, f);
	return valid;
}

// Clip `*x', `*y', `*w', `*h' to the current mode; 0 when nothing is left.
// Print-free (the console flush hook): out-of-bounds rects are dropped.
static int svga_update_clip(uint32_t *x, uint32_t *y, uint32_t *w,
			    uint32_t *h)
{
	uint32_t width = g_svga.crtc.width, height = g_svga.crtc.height;

	if (*w == 0 || *h == 0 || *x >= width || *y >= height)
		return 0;
	if (*w > width - *x)
		*w = width - *x;
	if (*h > height - *y)
		*h = height - *y;
	return 1;
}

// One SVGA_CMD_UPDATE, never waiting: 0 (queued, or nothing to send),
// -EAGAIN (the FIFO cannot take it right now) or -EIO (refused).
static int svga_update_send(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	uint32_t cmd[5];

	if (!svga_update_clip(&x, &y, &w, &h))
		return 0;
	cmd[0] = SVGA_CMD_UPDATE;
	cmd[1] = x;
	cmd[2] = y;
	cmd[3] = w;
	cmd[4] = h;
	return svga_fifo_try_write_cmd(cmd, sizeof(cmd));
}

int svga_console_flush_pending(void)
{
	uint32_t x1, y1, x2, y2;
	int tries;

	// Pairs with the barrier below: a producer that has just dropped the
	// FIFO lock sees a box merged by an update that found the lock
	// taken, or that update sees the lock free and sends the box itself.
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (tries = 0; tries < 2; tries++) {
		if (!svga_dirty_take(&x1, &y1, &x2, &y2))
			return 0;
		// The console no longer drives the scan-out: nothing to show.
		if (!g_svga.active)
			return 0;
		if (svga_update_send(x1, y1, x2 - x1, y2 - y1) != -EAGAIN)
			return 0;
		svga_dirty_merge(x1, y1, x2, y2);
		// Still held: its holder looks at the box after letting go.
		// Free again (or the ring was full): one more try here.
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (spin_is_locked(&svga_fifo_lock))
			return -EAGAIN;
	}
	return -EAGAIN;
}

void vmsvga2_update_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	// Suspended: nothing would reach the screen; the resume redraws the
	// whole of it.
	if (!g_svga.active || g_svga.pm_suspended)
		return;
	// Clip to the current mode; callers pass framebuffer coordinates.
	if (!svga_update_clip(&x, &y, &w, &h))
		return;
	// Nothing pending: straight into the FIFO, if it takes it now.
	if (!g_svga_dirty.valid && svga_update_send(x, y, w, h) != -EAGAIN)
		return;
	// Behind an earlier update that has not gone out, or the FIFO was
	// not free: into the box, which goes out as one update.
	svga_dirty_merge(x, y, x + w, y + h);
	(void)svga_console_flush_pending();
}

void vmsvga2_update_full(void)
{
	if (!g_svga.active)
		return;
	vmsvga2_update_rect(0, 0, g_svga.crtc.width, g_svga.crtc.height);
}

// ===========================================================================
// Modesetting
// ===========================================================================

static int svga_bpp_supported(uint32_t bpp)
{
	// XRGB8888 always; RGB565 only when the host runs at 16bpp or
	// supports guest bpp emulation.
	if (bpp == 32)
		return 1;
	if (bpp == 16)
		return g_svga.host_bpp == 16 ||
		       (g_svga.caps & SVGA_CAP_8BIT_EMULATION) != 0;
	return 0;
}

// Whether a scan-out of `pitch' x `height' at `fb_offset' fits the memory it
// is read from, or why not.
//
// The legacy scan-out reads VRAM, so the bytes from the framebuffer offset to
// the end of the last line must lie below the end of it.  A device that
// states how much memory its primary surfaces may take bounds the scan-out
// by that as well.
static const char *svga_mode_vram_reject(uint64_t pitch, uint64_t height,
					 uint64_t fb_offset)
{
	uint64_t bytes = pitch * height;

	if (!(fb_offset + bytes < (uint64_t)g_svga.vram_size))
		return "more video memory than the adapter has";
	if (g_svga_info.max_primary_mem &&
	    bytes > (uint64_t)g_svga_info.max_primary_mem)
		return "more than the device allows a primary surface";
	return NULL;
}

// Why the console cannot run `width' x `height' at `bpp', or NULL.  The pitch
// is the packed one the mode set locks, the offset the device's current one.
static const char *svga_mode_check(uint32_t width, uint32_t height,
				   uint32_t bpp)
{
	if (!g_svga.present)
		return "no device";
	if (width == 0 || height == 0)
		return "empty";
	if (width > g_svga.max_width || height > g_svga.max_height)
		return "beyond the geometry the adapter reports";
	if (!svga_bpp_supported(bpp))
		return "pixel size not supported";
	return svga_mode_vram_reject((uint64_t)width * (bpp / 8), height,
				     g_svga.crtc.fb_offset);
}

// The registers a mode set writes, as they were before it: what a refused
// mode set puts back, so the scan-out that was up -- the firmware's, at boot
// -- stays valid.  The topology is written only after a mode set succeeded,
// so it is not part of this.
struct svga_mode_regs {
	uint32_t width;
	uint32_t height;
	uint32_t bpp;
	uint32_t pitchlock;
	uint32_t enable;
};

// The mode the device was in at probe: where a failed boot takeover returns.
static struct svga_mode_regs svga_probe_mode;

static void svga_mode_regs_save(struct svga_mode_regs *r)
{
	r->width = svga_read_reg(SVGA_REG_WIDTH);
	r->height = svga_read_reg(SVGA_REG_HEIGHT);
	r->bpp = svga_read_reg(SVGA_REG_BITS_PER_PIXEL);
	r->pitchlock = svga_read_pitchlock();
	r->enable = svga_read_reg(SVGA_REG_ENABLE);
}

static void svga_mode_regs_restore(const struct svga_mode_regs *r)
{
	svga_write_pitchlock(r->pitchlock);
	svga_write_reg(SVGA_REG_WIDTH, r->width);
	svga_write_reg(SVGA_REG_HEIGHT, r->height);
	if (g_svga.caps & SVGA_CAP_8BIT_EMULATION)
		svga_write_reg(SVGA_REG_BITS_PER_PIXEL, r->bpp);
	// Only when it differs: writing 0 here resets the device.
	if (svga_read_reg(SVGA_REG_ENABLE) != r->enable)
		svga_write_reg(SVGA_REG_ENABLE, r->enable);
}

// The channel layout of a scan-out the console draws, as the device should
// report it after a mode set at `bpp'.
static void svga_expected_masks(uint32_t bpp, uint32_t *r, uint32_t *g,
				uint32_t *b)
{
	if (bpp == 16) {
		*r = 0xF800;
		*g = 0x07E0;
		*b = 0x001F;
	} else {
		*r = 0x00FF0000;
		*g = 0x0000FF00;
		*b = 0x000000FF;
	}
}

// Program a mode; caller holds svga_modeset_mutex.
//
// The mode registers are written with the pitch locked to the packed line
// size and the device's colour depth checked (svga_kms_write_svga()), the
// device is enabled if it is not, the geometry the device accepted is read
// back and checked, and only then is the topology written and the result
// made the current mode.  A refusal anywhere puts the registers back as they
// were and leaves the current mode alone, and says why.
//
// reset: cycle SVGA_REG_ENABLE through 0 first, which resets the device (see
// VMSVGA2_ENABLE_TOGGLE, which forces it).
static int svga_program_mode(uint32_t width, uint32_t height, uint32_t bpp,
			     int reset)
{
	struct svga_mode_regs saved;
	uint32_t eff_bpp, rb_w, rb_h, rb_bpp, pitch, fb_offset, fb_size;
	uint32_t depth, red, green, blue, want_r, want_g, want_b;
	const char *why = NULL;

#if VMSVGA2_ENABLE_TOGGLE
	reset = 1;
#endif
	svga_mode_regs_save(&saved);
	if (reset)
		svga_scanout_disable(0);

	// Without pixel-size emulation the guest runs at the host's size,
	// whatever it asks for; the console then draws at that size, as it
	// always has here, provided it is one it can draw.
	eff_bpp = bpp;
	if (!(g_svga.caps & SVGA_CAP_8BIT_EMULATION)) {
		eff_bpp = svga_read_reg(SVGA_REG_BITS_PER_PIXEL);
		if (eff_bpp != bpp) {
			if (!svga_format_depth(eff_bpp)) {
				why = "the host runs at a pixel size the "
				      "console cannot draw";
				goto refuse;
			}
			kprintf("svga2: no pixel-size emulation: %u bpp "
				"asked for, host runs at %u\n",
				bpp, eff_bpp);
		}
	}

	if (svga_kms_write_svga(width, height, width * (eff_bpp / 8), eff_bpp,
				svga_format_depth(eff_bpp)) != 0) {
		why = "colour depth mismatch";
		goto refuse;
	}
	svga_scanout_enable();

	// Read back what the host actually accepted.
	rb_w = svga_read_reg(SVGA_REG_WIDTH);
	rb_h = svga_read_reg(SVGA_REG_HEIGHT);
	rb_bpp = svga_read_reg(SVGA_REG_BITS_PER_PIXEL);
	pitch = svga_read_reg(SVGA_REG_BYTES_PER_LINE);
	fb_offset = svga_read_reg(SVGA_REG_FB_OFFSET);
	fb_size = svga_read_reg(SVGA_REG_FB_SIZE);

	if (WARN(rb_w != width || rb_h != height,
		 "svga2: host adjusted mode %ux%u -> %ux%u", width, height,
		 rb_w, rb_h)) {
		// Keep going with the host-adjusted geometry.
	}
	WARN_ON_ONCE(rb_bpp != eff_bpp); // host refused the pixel format
	if (!svga_format_depth(rb_bpp)) {
		why = "the host settled on a pixel size the console cannot "
		      "draw";
		goto refuse;
	}

	// Pitch handling: the line stride may exceed width*bytes-per-pixel.
	if (WARN_ON_ONCE(pitch < rb_w * (rb_bpp / 8))) {
		why = "line pitch below the line size";
		goto refuse;
	}
	// DMA/scanout bound check against VRAM.
	why = svga_mode_vram_reject(pitch, rb_h, fb_offset);
	if (WARN(why != NULL, "svga2: mode exceeds VRAM"))
		goto refuse;

	// The channel layout.  RGB565 is what the console (and /dev/fb0)
	// draws at 16 bpp, so a host that lays 16-bit pixels out otherwise is
	// refused rather than shown in the wrong colours.  At 32 bpp an
	// unexpected layout is only reported: that scan-out is still usable.
	depth = svga_read_reg(SVGA_REG_DEPTH);
	red = svga_read_reg(SVGA_REG_RED_MASK);
	green = svga_read_reg(SVGA_REG_GREEN_MASK);
	blue = svga_read_reg(SVGA_REG_BLUE_MASK);
	svga_expected_masks(rb_bpp, &want_r, &want_g, &want_b);
	if ((red | green | blue) != 0 &&
	    (red != want_r || green != want_g || blue != want_b)) {
		if (rb_bpp == 16) {
			why = "the host's 16-bit pixel layout is not RGB565";
			goto refuse;
		}
		if (!g_svga_info.masks_reported) {
			g_svga_info.masks_reported = 1;
			kprintf("svga2: unexpected channel masks "
				"r=0x%x g=0x%x b=0x%x at %u bpp\n",
				red, green, blue, rb_bpp);
		}
	}

	svga_ldu_commit_topology(rb_w, rb_h);

	g_svga.crtc.width = rb_w;
	g_svga.crtc.height = rb_h;
	g_svga.crtc.bpp = rb_bpp;
	g_svga.crtc.pitch = pitch;
	g_svga.crtc.fb_offset = fb_offset;
	g_svga.fb_size = fb_size;
	g_svga.crtc.enabled = 1;
	g_svga_info.depth = depth;
	g_svga_info.red_mask = red;
	g_svga_info.green_mask = green;
	g_svga_info.blue_mask = blue;
	return 0;

refuse:
	svga_mode_regs_restore(&saved);
	kprintf("svga2: mode %ux%ux%u refused: %s\n", width, height, bpp,
		why);
	return -1;
}

// Mode set with the device reset (`reset') or without; see vmsvga2_hw.c for
// which entry points ask for which.
int svga_set_mode(uint32_t width, uint32_t height, uint32_t bpp, int reset)
{
	const char *why;
	int rc;

	if (!g_svga.present)
		return -1;
	why = svga_mode_check(width, height, bpp);
	if (why) {
		kprintf("svga2: mode %ux%ux%u refused: %s\n", width, height,
			bpp, why);
		return -1;
	}

	svga_mutex_lock(&svga_modeset_mutex);
	if (g_svga.pm_suspended) {
		// The device may be reset, its FIFO stopped: a mode set now
		// would half bring it back.  The resume sets the console's
		// mode; a display manager sets its own after its resume.
		svga_mutex_unlock(&svga_modeset_mutex);
		kprintf("svga2: mode %ux%ux%u refused: device suspended\n",
			width, height, bpp);
		return -1;
	}
	rc = svga_program_mode(width, height, bpp, reset);
	if (rc == 0) {
		g_svga.active = 1;
		fbdev_register_backend(&vmsvga2_fbdev_backend);
	}
	svga_mutex_unlock(&svga_modeset_mutex);
	svga_report_errors(); // safe context: surface deferred FIFO errors
	return rc;
}

int vmsvga2_set_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
	return svga_set_mode(width, height, bpp, 0);
}

// Host-side dirty tracking of direct VRAM writes.  With traces enabled the
// host snoops framebuffer stores and repaints without explicit UPDATE
// commands - required for /dev/fb0 mmap clients (X.org fbdev style) that
// scan out by writing VRAM directly.  The explicit update-rect path stays
// correct alongside it; traces just add host-side tracking.
void vmsvga2_set_traces(int enable)
{
	if (!g_svga.present)
		return;
#if VMSVGA2_PM
	// Suspended: the device may be reset; the resume writes this.
	if (g_svga.pm_suspended) {
		g_svga_pm.traces = enable ? 1 : 0;
		return;
	}
#endif
	svga_write_reg(SVGA_REG_TRACES, enable ? 1 : 0);
}

// Does turning the legacy scan-out off by disabling the device lose state
// somebody holds?  On a device with command buffers, screen objects or
// guest-backed objects it does -- a display manager's contexts, screens and
// object tables go with the reset -- so blanking hides the framebuffer
// instead.  A device with none of those (QEMU) is blanked by disabling it,
// as it always was.
#if !VMSVGA2_ENABLE_TOGGLE
static int svga_blank_hides(void)
{
	return (g_svga.caps & (SVGA_CAP_COMMAND_BUFFERS |
			       SVGA_CAP_SCREEN_OBJECT_2 |
			       SVGA_CAP_GBOBJECTS)) != 0;
}
#endif

int vmsvga2_display_enable(int enable)
{
	if (!g_svga.present)
		return -1;
	svga_mutex_lock(&svga_modeset_mutex);
	if (g_svga.pm_suspended) {
		// SVGA_REG_ENABLE belongs to the suspend until the resume.
		svga_mutex_unlock(&svga_modeset_mutex);
		return -1;
	}
#if VMSVGA2_ENABLE_TOGGLE
	svga_write_reg(SVGA_REG_ENABLE, enable ? 1 : 0);
#else
	if (enable)
		svga_scanout_enable();
	else
		svga_scanout_disable(svga_blank_hides());
#endif
	g_svga.crtc.enabled = !!enable;
	svga_mutex_unlock(&svga_modeset_mutex);
	if (enable)
		vmsvga2_update_full();
	return 0;
}

/* SVGA_REG_ENABLE for the display-manager driver, without a mode set: see
 * vmsvga2_hw.h for what each value does.  The console's mode sets and its
 * blanking use the same two register sequences (svga_scanout_enable/
 * _disable). */
void vmsvga2_hw_svga_enable(void)
{
	if (!g_svga.present)
		return;
	svga_mutex_lock(&svga_modeset_mutex);
	// Ignored while suspended: SVGA_REG_ENABLE belongs to the suspend
	// until the resume.
	if (!g_svga.pm_suspended) {
		svga_scanout_enable();
		g_svga.crtc.enabled = 1;
	}
	svga_mutex_unlock(&svga_modeset_mutex);
}

void vmsvga2_hw_svga_disable(bool hide)
{
	if (!g_svga.present)
		return;
	svga_mutex_lock(&svga_modeset_mutex);
	if (!g_svga.pm_suspended) {
		svga_scanout_disable(hide);
		g_svga.crtc.enabled = 0;
	}
	svga_mutex_unlock(&svga_modeset_mutex);
}

int vmsvga2_get_info(framebuffer_info_t *out)
{
	if (!g_svga.active || !out)
		return -1;
	out->framebuffer_base =
		(void *)(g_svga.fb_virt + g_svga.crtc.fb_offset);
	out->framebuffer_size =
		g_svga.crtc.pitch * g_svga.crtc.height;
	out->horizontal_resolution = g_svga.crtc.width;
	out->vertical_resolution = g_svga.crtc.height;
	out->pixels_per_scanline = g_svga.crtc.pitch / (g_svga.crtc.bpp / 8);
	out->bytes_per_pixel = g_svga.crtc.bpp / 8;
	return 0;
}

uint64_t vmsvga2_get_fb_phys(uint64_t *size_out)
{
	if (!g_svga.present)
		return 0;
	if (size_out)
		*size_out = g_svga.vram_size;
	return g_svga.fb_phys;
}

// ===========================================================================
// Error recovery / device reset
// ===========================================================================

static int svga_negotiate_version(void);

// Full reinitialization after FIFO corruption, VM reset or resume.  Re-runs
// version negotiation, FIFO setup, the current mode and the cursor image.
//
// Not while a display manager drives the device through command buffers
// (it has registered its command channel, vmsvga2_set_cmd_channel()): the
// reset starts by disabling the device, which stops that driver's contexts
// and drops its guest-backed objects, and a FIFO this layer no longer
// submits to is not worth that.  The refusal is logged instead, a few times.
int vmsvga2_reset(void)
{
	static uint32_t refused;
	uint32_t w, h, bpp;
	int rc = 0;

	if (!g_svga.present)
		return -1;

	if (g_cmd_channel) {
		refused++;
		if (refused <= 4)
			kprintf("svga2: device reset skipped: a command-buffer "
				"owner drives the device (%u)\n",
				refused);
		return -1;
	}

	svga_mutex_lock(&svga_modeset_mutex);
	if (g_svga.pm_suspended) {
		// The resume re-initializes the device; resetting it in
		// between would only undo the suspend's drain.
		svga_mutex_unlock(&svga_modeset_mutex);
		return -1;
	}
	w = g_svga.crtc.width;
	h = g_svga.crtc.height;
	bpp = g_svga.crtc.bpp;

	// The reset proper: disabling the device is what resets it.
	svga_scanout_disable(0);
	if (svga_negotiate_version() != 0) {
		svga_mutex_unlock(&svga_modeset_mutex);
		WARN(1, "svga2: reset: device vanished");
		return -1;
	}

	// Reset FIFO cursors and reconfigure (the mapping is unchanged).
	svga_fifo_reset_ring();
	svga_write_reg(SVGA_REG_CONFIG_DONE, 1);
	svga_write_reg(SVGA_REG_GUEST_ID, SVGA_GUEST_ID_OTHER);
	if (g_svga.irq_enabled)
		svga_irq_rearm(); // re-arm, never silence: see svga_irq_rearm()

	if (g_svga.active && w && h)
		rc = svga_program_mode(w, h, bpp, 0);
	svga_mutex_unlock(&svga_modeset_mutex);

	// Restore the cursor image if one was defined.
	svga_cursor_restore();

	if (rc == 0 && g_svga.active)
		vmsvga2_update_full();
	svga_report_errors();
	return rc;
}


// ===========================================================================
// Capability / limit reporting
// ===========================================================================

/* ---- /dev/fb0 backend --------------------------------------------------- */

static void vmsvga2_fb_mapped(void)
{
	/* A mapping client scans out by storing straight to VRAM and sends
	 * no update commands.  Traces make the host snoop those writes;
	 * sticky-on -- the console's explicit update-rect path remains
	 * correct alongside it, at a small host-side tracking cost once a
	 * client has mapped the framebuffer. */
	vmsvga2_set_traces(1);
}

// The same checks a mode set makes before it touches the device: geometry,
// pixel size, and the packed scan-out at the device's framebuffer offset
// against VRAM (and the primary-surface limit, where the device has one).
static int vmsvga2_fb_test_mode(uint32_t w, uint32_t h, uint32_t bpp)
{
	const char *why = svga_mode_check(w, h, bpp);

	if (why) {
		kprintf("svga2: mode %ux%ux%u would be refused: %s\n", w, h,
			bpp, why);
		return -EINVAL;
	}
	return 0;
}

static const struct fbdev_backend vmsvga2_fbdev_backend = {
	.id = "svga2",
	.get_info = vmsvga2_get_info,
	.get_phys = vmsvga2_get_fb_phys,
	.mapped = vmsvga2_fb_mapped,
	.test_mode = vmsvga2_fb_test_mode,
	.set_mode = vmsvga2_set_mode,
	.blank = vmsvga2_display_enable,
	.update_full = vmsvga2_update_full,
};

int vmsvga2_active(void)
{
	return g_svga.active;
}

uint32_t vmsvga2_get_caps(void)
{
	return g_svga.caps;
}

uint32_t vmsvga2_get_vram_size(void)
{
	return g_svga.vram_size;
}

uint32_t vmsvga2_get_max_width(void)
{
	return g_svga.max_width;
}

uint32_t vmsvga2_get_max_height(void)
{
	return g_svga.max_height;
}


// ===========================================================================
// Host-preferred ("recommended") resolution
// ===========================================================================
//
// What the guest should come up in is the host's business, and the boot
// firmware is the wrong place to ask.  The GOP mode list is whatever the
// virtual firmware chose to advertise -- a handful of stock sizes, usually
// topping out well below the panel -- and it says nothing about the display
// the virtual machine's window is actually on.  The hypervisor knows, and
// answers in one of two ways:
//
//   1. The backdoor: a port-I/O protocol on 0x5658 entered with 'VMXh' in
//      EAX (include/kernel/hal/vmware_backdoor.h; the same channel
//      kernel/dev/gpu/vmwgfx/vmw_msg.c speaks for RPCI, here in its
//      simplest single-command form).  Command 15 returns
//      the host's screen size packed into EAX -- width in the high half,
//      height in the low.  This is the size the VMware tools call the
//      recommended resolution: the host's panel, not the guest's window.
//
//   2. The display topology registers, when the device advertises them.  At
//      reset, display 0 holds the layout the host wants the guest to come up
//      in, which on a single-monitor guest is the same answer.
//
// Both are read once, at probe, before anything programs a mode: the
// topology registers carry the host's layout only until the guest writes a
// topology of its own into them, after which they read back the guest's.
//
// Neither source exists everywhere -- QEMU answers no screen-size command
// and hands back the magic for commands it does not know -- so both are
// best-effort, and every answer is bounds-checked before it is believed.
// Nothing else depends on getting one: without an answer the driver falls
// back to its built-in preference list.

// Plausibility bounds for a screen size.  Anything outside them is a host
// that did not answer -- a bus reading back all ones, an echoed magic, a
// register that was never implemented -- rather than a display to configure.
#define SVGA_PREF_MIN_WIDTH 640U
#define SVGA_PREF_MIN_HEIGHT 480U
#define SVGA_PREF_MAX_DIM 16384U

static int svga_pref_plausible(uint32_t w, uint32_t h)
{
	return w >= SVGA_PREF_MIN_WIDTH && h >= SVGA_PREF_MIN_HEIGHT &&
	       w <= SVGA_PREF_MAX_DIM && h <= SVGA_PREF_MAX_DIM;
}

// The host's screen size over the backdoor.  Only ever called with the SVGA
// II adapter present, so port 0x5658 belongs to the hypervisor and not to
// some unrelated ISA device.
static int svga_backdoor_screen_size(uint32_t *w, uint32_t *h)
{
	// All ones: the protocol's refusal, and also what an unclaimed port
	// reads back.  The magic returned unchanged: a hypervisor that does
	// not implement this command.  Both come back as -1.
	return vmware_backdoor_screen_size(w, h);
}

// The host's layout for display 0, before any guest modeset has overwritten
// the topology registers.
//
// Discounted when it merely repeats the mode the device is already in.  The
// topology registers are guest-writable, and on a UEFI guest the firmware
// has already programmed this device through the very same register file to
// put its GOP framebuffer up -- so a topology equal to the current scanout
// is the firmware's choice being read back, which is precisely the answer
// this whole path exists not to take.  The backdoor is not second-guessed
// this way: there the host is unambiguously the one talking.
static int svga_topology_screen_size(uint32_t *w, uint32_t *h)
{
	uint32_t width, height;

	if (!(g_svga.caps & SVGA_CAP_DISPLAY_TOPOLOGY))
		return -1;
	svga_write_reg(SVGA_REG_DISPLAY_ID, 0);
	width = svga_read_reg(SVGA_REG_DISPLAY_WIDTH);
	height = svga_read_reg(SVGA_REG_DISPLAY_HEIGHT);
	if (width == 0 || height == 0)
		return -1;
	g_svga.topo_width = width;
	g_svga.topo_height = height;
	if (width == svga_read_reg(SVGA_REG_WIDTH) &&
	    height == svga_read_reg(SVGA_REG_HEIGHT))
		return -1;
	*w = width;
	*h = height;
	return 0;
}

// Poll both sources once and remember the first plausible answer.
static void svga_query_host_preferred(void)
{
	uint32_t w = 0, h = 0;

	g_svga.pref_width = 0;
	g_svga.pref_height = 0;
	g_svga.pref_source = NULL;

	if (svga_backdoor_screen_size(&w, &h) == 0 &&
	    svga_pref_plausible(w, h)) {
		g_svga.pref_source = "host screen";
	} else if (svga_topology_screen_size(&w, &h) == 0 &&
		   svga_pref_plausible(w, h)) {
		g_svga.pref_source = "host topology";
	} else {
		return;
	}
	g_svga.pref_width = w;
	g_svga.pref_height = h;
}

int vmsvga2_get_host_preferred(uint32_t *width, uint32_t *height)
{
	if (!g_svga.present || g_svga.pref_width == 0 ||
	    g_svga.pref_height == 0)
		return -1;
	if (width)
		*width = g_svga.pref_width;
	if (height)
		*height = g_svga.pref_height;
	return 0;
}

// ===========================================================================
// Device bring-up
// ===========================================================================

static const pci_device_t *svga_find_pci_device(void)
{
	int count = 0;
	const pci_device_t *devs = pci_get_devices(&count);
	int i;

	for (i = 0; i < count; i++) {
		if (devs[i].vendor_id == SVGA_PCI_VENDOR_ID &&
		    devs[i].device_id == SVGA_PCI_DEVICE_ID)
			return &devs[i];
	}
	return NULL;
}

static int svga_negotiate_version(void)
{
	static const uint32_t ids[] = { SVGA_ID_2, SVGA_ID_1, SVGA_ID_0 };
	unsigned int i;

	for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		svga_write_reg(SVGA_REG_ID, ids[i]);
		if (svga_read_reg(SVGA_REG_ID) == ids[i]) {
			g_svga.version = ids[i] & 0xFF;
			return 0;
		}
	}
	return -1;
}

// Names of the capability bits, for the probe log.
struct svga_bitmap_name {
	uint32_t value;
	const char *name;
};

static const struct svga_bitmap_name svga_cap1_names[] = {
	{ SVGA_CAP_RECT_COPY, "rect copy" },
	{ SVGA_CAP_CURSOR, "cursor" },
	{ SVGA_CAP_CURSOR_BYPASS, "cursor bypass" },
	{ SVGA_CAP_CURSOR_BYPASS_2, "cursor bypass 2" },
	{ SVGA_CAP_8BIT_EMULATION, "8bit emulation" },
	{ SVGA_CAP_ALPHA_CURSOR, "alpha cursor" },
	{ SVGA_CAP_3D, "3D" },
	{ SVGA_CAP_EXTENDED_FIFO, "extended fifo" },
	{ SVGA_CAP_MULTIMON, "multimon" },
	{ SVGA_CAP_PITCHLOCK, "pitchlock" },
	{ SVGA_CAP_IRQMASK, "irq mask" },
	{ SVGA_CAP_DISPLAY_TOPOLOGY, "display topology" },
	{ SVGA_CAP_GMR, "gmr" },
	{ SVGA_CAP_TRACES, "traces" },
	{ SVGA_CAP_GMR2, "gmr2" },
	{ SVGA_CAP_SCREEN_OBJECT_2, "screen object 2" },
	{ SVGA_CAP_COMMAND_BUFFERS, "command buffers" },
	{ SVGA_CAP_CMD_BUFFERS_2, "command buffers 2" },
	{ SVGA_CAP_GBOBJECTS, "gbobject" },
	{ SVGA_CAP_DX, "dx" },
	{ SVGA_CAP_HP_CMD_QUEUE, "hp cmd queue" },
	{ SVGA_CAP_NO_BB_RESTRICTION, "no bb restriction" },
	{ SVGA_CAP_CAP2_REGISTER, "cap2 register" },
};

static const struct svga_bitmap_name svga_cap2_names[] = {
	{ SVGA_CAP2_GROW_OTABLE, "grow otable" },
	{ SVGA_CAP2_INTRA_SURFACE_COPY, "intra surface copy" },
	{ SVGA_CAP2_DX2, "dx2" },
	{ SVGA_CAP2_GB_MEMSIZE_2, "gb memsize 2" },
	{ SVGA_CAP2_SCREENDMA_REG, "screendma reg" },
	{ SVGA_CAP2_OTABLE_PTDEPTH_2, "otable ptdepth2" },
	{ SVGA_CAP2_NON_MS_TO_MS_STRETCHBLT, "non ms to ms stretchblt" },
	{ SVGA_CAP2_CURSOR_MOB, "cursor mob" },
	{ SVGA_CAP2_MSHINT, "mshint" },
	{ SVGA_CAP2_CB_MAX_SIZE_4MB, "cb max size 4mb" },
	{ SVGA_CAP2_DX3, "dx3" },
	{ SVGA_CAP2_FRAME_TYPE, "frame type" },
	{ SVGA_CAP2_COTABLE_COPY, "cotable copy" },
	{ SVGA_CAP2_TRACE_FULL_FB, "trace full fb" },
	{ SVGA_CAP2_EXTRA_REGS, "extra regs" },
	{ SVGA_CAP2_LO_STAGING, "lo staging" },
	{ SVGA_CAP2_VIDEO_BLT, "video blt" },
	{ SVGA_CAP2_MOBFMT_16K_PT64, "mobfmt 16k pt64" },
	{ SVGA_CAP2_MOB_FENCE_WITH_FLAGS, "mob fence with flags" },
	{ SVGA_CAP2_DIRTY_TRACKING_REG, "dirty tracking reg" },
	{ SVGA_CAP2_CURSOR_MOB_ALPHA3D, "cursor mob alpha3d" },
};

// One line naming the bits set in `bitmap', plus the hex of any bit without
// a name.  Probe time only: the line is built in a static buffer (too large
// for the stack), and nothing else runs this concurrently.
static void svga_print_bitmap(const char *prefix, uint32_t bitmap,
			      const struct svga_bitmap_name *names,
			      uint32_t num_names)
{
	static char buf[512];
	size_t off = 0;
	uint32_t i;

	buf[0] = '\0';
	for (i = 0; i < num_names && off + 1 < sizeof(buf); i++) {
		if (!(bitmap & names[i].value))
			continue;
		ksnprintf(buf + off, sizeof(buf) - off, "%s%s",
			  off ? ", " : "", names[i].name);
		off = kstrlen(buf);
		bitmap &= ~names[i].value;
	}
	if (bitmap && off + 1 < sizeof(buf))
		ksnprintf(buf + off, sizeof(buf) - off, "%sunknown 0x%x",
			  off ? ", " : "", bitmap);
	kprintf("svga2: %s: %s\n", prefix, buf[0] ? buf : "none");
}

// SVGA_REG_ENABLE, _CONFIG_DONE and _TRACES as the firmware left them,
// before this driver writes any of them; vmsvga2_shutdown() puts them back.
static void svga_save_device_state(void)
{
	g_svga_info.saved_enable = svga_read_reg(SVGA_REG_ENABLE);
	g_svga_info.saved_config_done = svga_read_reg(SVGA_REG_CONFIG_DONE);
	g_svga_info.saved_traces = svga_read_reg(SVGA_REG_TRACES);
	g_svga_info.state_saved = 1;
}

// The device's limits, read once: they do not change while it runs.  The
// one place SVGA_REG_CAP2 and the region limits are read; the interrupt
// (fence-goal source), the cursor (CURSOR4 registers) and the guest memory
// regions go by what this found (svga_device_caps2(),
// svga_device_gmr_limits()).
static void svga_read_device_limits(void)
{
	if (g_svga.caps & SVGA_CAP_CAP2_REGISTER)
		g_svga_info.caps2 = svga_read_reg(SVGA_REG_CAP2);
	else
		g_svga_info.caps2 = 0;

	// The id limit exists with either kind of guest memory region: the
	// descriptor-programmed regions hand out ids just the same.
	if (g_svga.caps & (SVGA_CAP_GMR | SVGA_CAP_GMR2))
		g_svga_info.gmr_max_ids = svga_read_reg(SVGA_REG_GMR_MAX_IDS);
	else
		g_svga_info.gmr_max_ids = 0;

	if (g_svga.caps & SVGA_CAP_GMR2) {
		g_svga_info.gmr_max_pages =
			svga_read_reg(SVGA_REG_GMRS_MAX_PAGES);
		g_svga_info.memory_size = svga_read_reg(SVGA_REG_MEMORY_SIZE);
		// The register counts VRAM in; what is left is for guest
		// memory regions.  A host answering less than VRAM leaves
		// nothing rather than wrapping.
		if (g_svga_info.memory_size > g_svga.vram_size)
			g_svga_info.memory_size -= g_svga.vram_size;
		else
			g_svga_info.memory_size = 0;
	} else {
		// No limit stated: an arbitrary 512 MiB of surface memory.
		g_svga_info.gmr_max_pages = 0;
		g_svga_info.memory_size = 512U * 1024U * 1024U;
	}

	g_svga_info.max_primary_mem = g_svga.vram_size;
	if (g_svga.caps & SVGA_CAP_GBOBJECTS) {
		uint32_t prim = svga_read_reg(SVGA_REG_MAX_PRIMARY_MEM);
		// 0 would refuse every mode: a register that is not there.
		if (prim)
			g_svga_info.max_primary_mem = prim;
	}
	__asm__ volatile("" ::: "memory"); // the values before the flag
	g_svga_info.limits_read = 1;
}

// The limits as read at probe -- or now, by whoever asks first, when probe
// has the capabilities (and the VRAM size the memory figure is computed
// from) but has not read them yet.  Before that there is nothing to read
// them by: no CAP2, no regions.
static int svga_device_limits_ready(void)
{
	if (g_svga_info.limits_read)
		return 1;
	if (!g_svga.io_base || !g_svga.caps || !g_svga.vram_size)
		return 0;
	svga_read_device_limits();
	return 1;
}

uint32_t svga_device_caps2(void)
{
	return svga_device_limits_ready() ? g_svga_info.caps2 : 0;
}

void svga_device_gmr_limits(uint32_t *max_ids, uint32_t *max_pages)
{
	int ready = svga_device_limits_ready();

	if (max_ids)
		*max_ids = ready ? g_svga_info.gmr_max_ids : 0;
	if (max_pages)
		*max_pages = ready ? g_svga_info.gmr_max_pages : 0;
}

// The size to come up in when nothing better is known: the mode registers,
// raised to the minimum a virtual machine window is expected to hold, and
// that minimum when they read beyond the device's own maximum -- a host
// error.  Read at probe, before any mode set of ours.
static void svga_get_initial_size(void)
{
	uint32_t width = svga_read_reg(SVGA_REG_WIDTH);
	uint32_t height = svga_read_reg(SVGA_REG_HEIGHT);

	if (width < SVGA_MIN_INITIAL_WIDTH)
		width = SVGA_MIN_INITIAL_WIDTH;
	if (height < SVGA_MIN_INITIAL_HEIGHT)
		height = SVGA_MIN_INITIAL_HEIGHT;
	if (width > g_svga.max_width || height > g_svga.max_height) {
		width = SVGA_MIN_INITIAL_WIDTH;
		height = SVGA_MIN_INITIAL_HEIGHT;
	}
	g_svga_info.initial_width = width;
	g_svga_info.initial_height = height;
}

int vmsvga2_get_initial_size(uint32_t *width, uint32_t *height)
{
	if (!g_svga.present || !g_svga_info.initial_width)
		return -1;
	if (width)
		*width = g_svga_info.initial_width;
	if (height)
		*height = g_svga_info.initial_height;
	return 0;
}

uint32_t vmsvga2_get_caps2(void)
{
	return g_svga.present ? svga_device_caps2() : 0;
}

int vmsvga2_get_rgb_masks(uint32_t *red, uint32_t *green, uint32_t *blue)
{
	if (!g_svga.active)
		return -1;
	if (red)
		*red = g_svga_info.red_mask;
	if (green)
		*green = g_svga_info.green_mask;
	if (blue)
		*blue = g_svga_info.blue_mask;
	return 0;
}

int vmsvga2_init(void)
{
	const pci_device_t *dev;
	uint32_t cmd_reg;
	uint32_t bar0, bar1;

	BUILD_BUG_ON(sizeof(SVGAGuestMemDescriptor) != 8);

	svga_mutex_init(&svga_modeset_mutex, "svga_modeset");

	dev = svga_find_pci_device();
	if (!dev)
		return -1; // no SVGA II adapter: GOP fallback

	g_svga.pci = dev;

	// BAR0 must be an I/O BAR, BAR1 the framebuffer memory BAR.
	bar0 = dev->bar[0];
	bar1 = dev->bar[1];
	if (WARN_ON(!(bar0 & 1)) || WARN_ON(bar1 & 1))
		return -1;
	g_svga.io_base = (uint16_t)(bar0 & ~0x3U);
	g_svga.fb_phys = bar1 & ~0xFU;
	if (WARN_ON(g_svga.io_base == 0 || g_svga.fb_phys == 0))
		return -1;

	// Enable I/O, memory decoding and bus mastering.
	cmd_reg = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x04);
	pci_cfg_write32(dev->bus, dev->device, dev->function, 0x04,
			cmd_reg | 0x7);

	if (svga_negotiate_version() != 0) {
		kprintf("svga2: version negotiation failed\n");
		return -1;
	}

	if (g_svga.version < SVGA_VERSION_1) {
		// SVGA_ID_0 has no capability register or FIFO extensions;
		// not worth supporting on real hosts.
		kprintf("svga2: device version too old (%u)\n",
			g_svga.version);
		return -1;
	}

	// Before anything below writes them: what shutdown puts back.
	svga_save_device_state();

	g_svga.caps = svga_read_reg(SVGA_REG_CAPABILITIES);
	g_svga.vram_size = svga_read_reg(SVGA_REG_VRAM_SIZE);
	g_svga.max_width = svga_read_reg(SVGA_REG_MAX_WIDTH);
	g_svga.max_height = svga_read_reg(SVGA_REG_MAX_HEIGHT);
	g_svga.host_bpp = svga_read_reg(SVGA_REG_HOST_BITS_PER_PIXEL);
	g_svga.fb_size = svga_read_reg(SVGA_REG_FB_SIZE);
	svga_write_reg(SVGA_REG_GUEST_ID, SVGA_GUEST_ID_OTHER);

	if (WARN_ON(g_svga.vram_size == 0))
		return -1;

	// CAP2 and the memory limits, once; and the fallback size, from the
	// mode registers as the firmware left them.
	svga_read_device_limits();
	svga_get_initial_size();

	// Ask the host what it wants us in, while the answer is still the
	// host's: the topology registers hold its layout only until a guest
	// modeset writes over them, and that happens further down this boot.
	svga_query_host_preferred();

	// The framebuffer BAR lives below 4 GB and the direct map spans at
	// least 16 GB, so the direct-map alias is always available.
	BUG_ON(!is_phys_in_direct_map(g_svga.fb_phys));
	g_svga.fb_virt = (uint8_t *)phys_to_virt(g_svga.fb_phys);

	// Write-combining mapping for the framebuffer (same PAT machinery as
	// the GOP framebuffer path).
	if (configure_pat_write_combining(g_svga.fb_phys, g_svga.vram_size) !=
	    0)
		kprintf("svga2: WC mapping unavailable, using UC/WB\n");

	// Bounce buffer for FIFO reservations that wrap the ring, sized for
	// the largest command the FIFO accepts (svga_fifo_bounce_size()).
	// The FIFO size register is read here, ahead of svga_fifo_init(), so
	// the allocation stays where it always was in the bring-up order.  A
	// failed large allocation falls back to the minimum, and the FIFO then
	// refuses commands above that instead of overrunning it.
	g_svga.bounce_size =
		svga_fifo_bounce_size(svga_read_reg(SVGA_REG_MEM_SIZE));
	g_svga.bounce = kalloc(g_svga.bounce_size);
	if (!g_svga.bounce && g_svga.bounce_size > SVGA_FIFO_BOUNCE_MIN) {
		kprintf("svga2: no %u KB FIFO bounce buffer, using %u KB\n",
			g_svga.bounce_size / 1024, SVGA_FIFO_BOUNCE_MIN / 1024);
		g_svga.bounce_size = SVGA_FIFO_BOUNCE_MIN;
		g_svga.bounce = kalloc(g_svga.bounce_size);
	}
	if (!g_svga.bounce)
		return -1;

	if (svga_fifo_init() != 0) {
		kfree(g_svga.bounce);
		g_svga.bounce = NULL;
		return -1;
	}

	// The mode the device is in now -- the firmware's -- for a boot
	// takeover that has to be undone.  After the FIFO is up: the pitch
	// lock may live in it.
	svga_mode_regs_save(&svga_probe_mode);

	g_svga.next_fence = 0;
	g_svga.present = 1;
	g_vmsvga_initialized = 1;

	// Fence-completion interrupts (capability-gated; QEMU: polling).
	svga_irq_init();

	// Hardware cursor path and guest-memory-region limits (log + cache).
	svga_cursor_init();
	svga_gmr_init();

	kprintf("svga2: SVGA II v%u io=0x%x fb=0x%lx vram=%uKB fifo=%uKB\n",
		g_svga.version, g_svga.io_base,
		(unsigned long)g_svga.fb_phys, g_svga.vram_size / 1024,
		g_svga.fifo_size / 1024);
	kprintf("svga2: caps=0x%x fifo_caps=0x%x max=%ux%u host_bpp=%u\n",
		g_svga.caps, g_svga.fifo_caps, g_svga.max_width,
		g_svga.max_height, g_svga.host_bpp);
	svga_print_bitmap("capabilities", g_svga.caps, svga_cap1_names,
			  sizeof(svga_cap1_names) / sizeof(svga_cap1_names[0]));
	if (g_svga.caps & SVGA_CAP_CAP2_REGISTER)
		svga_print_bitmap("capabilities2", g_svga_info.caps2,
				  svga_cap2_names,
				  sizeof(svga_cap2_names) /
					  sizeof(svga_cap2_names[0]));
	kprintf("svga2: surface memory %uKB, primary up to %uKB, "
		"fallback size %ux%u\n",
		g_svga_info.memory_size / 1024,
		g_svga_info.max_primary_mem / 1024, g_svga_info.initial_width,
		g_svga_info.initial_height);
	if (g_svga.pref_width)
		kprintf("svga2: host recommends %ux%u (%s)\n",
			g_svga.pref_width, g_svga.pref_height,
			g_svga.pref_source);
	else if (g_svga.topo_width)
		kprintf("svga2: host recommends nothing: topology %ux%u only "
			"mirrors the firmware's mode\n",
			g_svga.topo_width, g_svga.topo_height);
	else
		kprintf("svga2: host recommends nothing: no backdoor answer, "
			"no topology\n");
	// The mode registers as found: on a UEFI guest they are the firmware's
	// GOP mode (VMware's EFI puts up 1024x768 whatever the host shows the
	// guest in), so they are logged, not believed.  Boot of 2026-09-09.
	kprintf("svga2: mode registers at probe %ux%u enable=%u\n",
		svga_read_reg(SVGA_REG_WIDTH), svga_read_reg(SVGA_REG_HEIGHT),
		svga_read_reg(SVGA_REG_ENABLE));

	return 0;
}

// Build-time ceiling on the preferred-resolution search (make MAX_SCREEN_SIZE=WxH).
// The device's reported maximum is what the emulated adapter can scan out, not
// what the panel behind it can show, so a virtual machine on a 1920x1080
// notebook happily offers 1920x1200 and the guest ends up on a screen the host
// then has to shrink.  This is the second bound: unset means no ceiling, and
// the bootloader applies the same one to the same table (boot/bootloader.c).
#ifndef SCREEN_MAX_WIDTH
#define SCREEN_MAX_WIDTH  0xFFFFFFFFU
#endif
#ifndef SCREEN_MAX_HEIGHT
#define SCREEN_MAX_HEIGHT 0xFFFFFFFFU
#endif

// What SCREEN_SIZE asks for: the top of the mode list, not the whole of it.
// The list below reaches higher than either of these so that a device which
// can only do a little still finds the best it can do; the ceiling is what
// keeps a device which can do a lot from coming up larger than the build
// asked for.
#if defined(SCREEN_LARGE)
#define SCREEN_PREF_MAX_WIDTH 1920U
#define SCREEN_PREF_MAX_HEIGHT 1200U
#else
#define SCREEN_PREF_MAX_WIDTH 1280U
#define SCREEN_PREF_MAX_HEIGHT 800U
#endif

// Why a mode cannot be used, or NULL when it can.
//
// The bounds are the ones that hold for this device: the
// geometry the adapter reports it can scan out, and the memory the scan-out
// reads from.  For this console that memory is the framebuffer aperture --
// it puts pixels in VRAM and the device reads them from there -- so VRAM is
// the honest limit here, and a small graphics-memory setting shows up as a
// small screen no matter what the panel in front of the user can do.  (The
// display manager scans out of guest memory instead and is bounded by
// something else entirely; see kernel/dev/gpu/vmwgfx/vmw_drv.c.)
static const char *svga_mode_reject(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0)
		return "empty";
	if (w > SCREEN_MAX_WIDTH || h > SCREEN_MAX_HEIGHT)
		return "above the build's MAX_SCREEN_SIZE";
	if (w > g_svga.max_width || h > g_svga.max_height)
		return "beyond the geometry the adapter reports";
	return svga_mode_vram_reject((uint64_t)w * 4, h,
				     g_svga.crtc.fb_offset);
}

static int svga_mode_usable(uint32_t w, uint32_t h)
{
	return svga_mode_reject(w, h) == NULL;
}

// ...and is one the build would pick on its own.  SCREEN_SIZE bounds what
// the mode list may offer; it does not overrule the host, which knows what
// it is showing this guest on.  MAX_SCREEN_SIZE, which does overrule it, is
// checked in svga_mode_reject() above and so applies to both.
static int svga_mode_preferable(uint32_t w, uint32_t h)
{
	return svga_mode_usable(w, h) && w <= SCREEN_PREF_MAX_WIDTH &&
	       h <= SCREEN_PREF_MAX_HEIGHT;
}

// Boot-time mode selection.  The host's own recommendation comes first --
// see the host-preferred section above -- so the mode the guest comes up in
// is the one the display in front of the user actually wants, whether or not
// the boot firmware ever advertised it: the GOP mode list bounds what the
// bootloader could pick, and nothing here.  Only when the hypervisor has no
// answer does this fall back to the built-in preference table, which is the
// same one (and the same SCREEN_LARGE build define) the bootloader walks.
//
// Either way the choice is bounded by the build-time ceiling, the device's
// maximum geometry and VRAM.  Takes over the display from the GOP
// framebuffer: console rendering continues through the same fb double-buffer
// path, with dirty flushes forwarded to the host as screen-update commands
// via the fb flush hook.
void vmsvga2_setup_boot_mode(void)
{
	// The standard mode list, largest first: the standard set of modes a
	// connector of this device offers, rather than the
	// handful of sizes the boot firmware happened to advertise.  The
	// first entry that clears svga_mode_reject() wins, so a device that
	// can only do a little still gets the best it can do -- which on a
	// machine with little graphics memory is a mode no short list would
	// have contained.
	static const struct {
		uint32_t width;
		uint32_t height;
	} builtin[] = {
		{ 2560, 1600 }, { 2560, 1440 }, { 1920, 1440 },
		{ 1920, 1200 }, { 1920, 1080 }, { 1856, 1392 },
		{ 1792, 1344 }, { 1680, 1050 }, { 1600, 1200 },
		{ 1600, 900 },	{ 1440, 900 },	{ 1400, 1050 },
		{ 1366, 768 },	{ 1360, 768 },	{ 1280, 1024 },
		{ 1280, 960 },	{ 1280, 800 },	{ 1280, 768 },
		{ 1280, 720 },	{ 1152, 864 },	{ 1024, 768 },
		{ 800, 600 },	{ 640, 480 },
	};
	framebuffer_info_t fi;
	uint32_t w = 0, h = 0;
	const char *source = "fallback";
	uint32_t fence;
	unsigned int i;

	if (!g_svga.present)
		return;

	// What the host recommends, if it said and if it fits.  A ceiling the
	// build asked for still wins: MAX_SCREEN_SIZE is an explicit "never
	// above this", not a guess for the host to correct.
	if (vmsvga2_get_host_preferred(&w, &h) == 0) {
		const char *why = svga_mode_reject(w, h);
		if (!why) {
			source = g_svga.pref_source;
		} else {
			kprintf("svga2: host recommends %ux%u, declined: %s\n",
				w, h, why);
			w = 0;
			h = 0;
		}
	}

	for (i = 0; w == 0 && i < sizeof(builtin) / sizeof(builtin[0]); i++) {
		if (svga_mode_preferable(builtin[i].width, builtin[i].height)) {
			w = builtin[i].width;
			h = builtin[i].height;
			source = "mode list";
		}
	}
	// Nothing on the list the build would pick: the fallback size worked
	// out at probe (the firmware's mode, raised to 1280x800), if the
	// device can carry it.  After the list, not before it -- the list is
	// where the build's SCREEN_SIZE applies, and the firmware's mode is
	// not to overrule that (see the host-preferred section).
	if (w == 0 && vmsvga2_get_initial_size(&w, &h) == 0) {
		if (svga_mode_usable(w, h)) {
			source = "fallback size";
		} else {
			w = 0;
			h = 0;
		}
	}
	if (w == 0) {
		// Nothing in the list fits (tiny VRAM, or a ceiling below every
		// entry): last resort, and small enough that the ceiling is not
		// worth honouring at the price of having no display at all.
		w = 640;
		h = 480;
		if (w > g_svga.max_width || h > g_svga.max_height ||
		    svga_mode_vram_reject((uint64_t)w * 4, h,
					  g_svga.crtc.fb_offset)) {
			kprintf("svga2: no usable mode, staying on GOP\n");
			return;
		}
	}

	// Take over scanout.  If the GOP already runs this exact geometry the
	// switch is visually seamless (same VRAM); the console re-renders its
	// scrollback view right after either way.
	if (vmsvga2_set_mode(w, h, 32) != 0) {
		kprintf("svga2: boot modeset %ux%u failed, staying on GOP\n",
			w, h);
		return;
	}
	if (WARN_ON_ONCE(vmsvga2_get_info(&fi) != 0))
		return;

	fb_set_flush_hook(vmsvga2_update_rect);
	if (console_reinit_framebuffer(&fi) != 0) {
		// Roll back: stop forwarding updates and leave the GOP
		// framebuffer path untouched -- which means the device has
		// to scan out the firmware's mode again, as it did at probe.
		fb_set_flush_hook(0);
		g_svga.active = 0;
#if VMSVGA2_ENABLE_TOGGLE
		svga_write_reg(SVGA_REG_ENABLE, 0);
#else
		svga_mutex_lock(&svga_modeset_mutex);
		svga_mode_regs_restore(&svga_probe_mode);
		svga_mutex_unlock(&svga_modeset_mutex);
#endif
		WARN(1, "svga2: console reinit failed, reverting to GOP");
		return;
	}
	vmsvga2_update_full();

	// FIFO/fence round-trip smoke test: catches a wedged host early.
	fence = vmsvga2_fence_insert();
	if (fence)
		(void)vmsvga2_fence_wait(fence, SVGA_FENCE_TIMEOUT_US);
	else
		(void)svga_legacy_sync();
	// Drained: whatever the console could not queue so far goes now.
	(void)svga_console_flush_pending();
	svga_report_errors();

	kprintf("svga2: display %ux%ux%u pitch=%u from %s (%s)\n",
		g_svga.crtc.width, g_svga.crtc.height, g_svga.crtc.bpp,
		g_svga.crtc.pitch, source,
		svga_has_fence() ? "fenced" : "sync-only");
}

// ===========================================================================
// Power management and shutdown
// ===========================================================================
//
// Taking the device down is the same few steps whether it comes back
// (vmsvga2_hw_suspend()) or not (vmsvga2_shutdown()), and so is bringing it
// up again the steps of probe, in probe's order:
//
//   down: drain the FIFO (a fence where one can be used, else SYNC/BUSY),
//         close it to further work and drain once more (svga_fifo_quiesce()),
//         stop the interrupt (mask 0, latched status acknowledged, line no
//         longer claimed); then, for a reset or for good, SVGA_REG_CONFIG_DONE,
//         SVGA_REG_ENABLE and SVGA_REG_TRACES -- CONFIG_DONE first, which stops
//         the FIFO, ENABLE 0 (a reset) or as found at probe (shutdown).
//   up:   PCI command bits, version, capabilities; when the device lost its
//         state: CONFIG_DONE 0 while the FIFO is laid out again, ENABLE,
//         TRACES, the FIFO, CONFIG_DONE 1, guest id, fence register; then the
//         interrupt, the cursor, the descriptor regions and the console's mode.
//
// What lives above this layer -- a display manager's guest-backed objects,
// command-buffer contexts, GMR2 regions, screens -- is its to rebuild after a
// resume that returned 1 (see vmsvga2_hw.h).

// SVGA_REG_CONFIG_DONE, _ENABLE and _TRACES for a device this driver lets go
// of: CONFIG_DONE and TRACES as found at probe, ENABLE `enable'.
// CONFIG_DONE first: once it is back to what the firmware left (0 unless the
// firmware ran the FIFO itself) the host no longer reads the FIFO.
static void svga_device_release(uint32_t enable)
{
	if (g_svga_info.state_saved) {
		svga_write_reg(SVGA_REG_CONFIG_DONE,
			       g_svga_info.saved_config_done);
		svga_write_reg(SVGA_REG_ENABLE, enable);
		svga_write_reg(SVGA_REG_TRACES, g_svga_info.saved_traces);
	} else {
		svga_write_reg(SVGA_REG_ENABLE, 0);
	}
}

#if VMSVGA2_PM
// The PCI command register bits probe sets: I/O space, memory space, bus
// mastering.
#define SVGA_PCI_COMMAND 0x04
#define SVGA_PCI_CMD_IO 0x1U
#define SVGA_PCI_CMD_MEMORY 0x2U
#define SVGA_PCI_CMD_MASTER 0x4U

static void svga_pci_command_update(uint32_t set, uint32_t clear)
{
	const pci_device_t *dev = g_svga.pci;
	uint32_t cmd = pci_cfg_read32(dev->bus, dev->device, dev->function,
				      SVGA_PCI_COMMAND);
	uint32_t want = (cmd | set) & ~clear;

	if (want != cmd)
		pci_cfg_write32(dev->bus, dev->device, dev->function,
				SVGA_PCI_COMMAND, want);
}

// The first half of a suspend and of a shutdown: everything queued drained,
// the FIFO closed, the interrupt stopped.  Caller holds svga_modeset_mutex.
static void svga_pm_quiesce(void)
{
	vmsvga2_fifo_flush();
	(void)svga_fifo_quiesce();
	g_svga_pm.fence = svga_has_fence() ? g_svga.fifo[SVGA_FIFO_FENCE] : 0;
	svga_irq_uninstall();
}

// Is `a' a later fence value than `b'?  Fence values wrap.
static int svga_fence_after(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) > 0;
}

// Did a device that was left running lose its state anyway (a host that
// reset it behind the guest's back)?  Why, or NULL when it looks as it was
// left.  Read before anything is written.
static const char *svga_pm_state_lost(void)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t next, stop;

	if (svga_read_reg(SVGA_REG_CONFIG_DONE) == 0)
		return "the FIFO is no longer configured";
	if ((g_svga_pm.enable & SVGA_REG_ENABLE_ENABLE) &&
	    !(svga_read_reg(SVGA_REG_ENABLE) & SVGA_REG_ENABLE_ENABLE))
		return "the device is no longer enabled";
	if (fifo[SVGA_FIFO_MIN] != g_svga.fifo_min ||
	    fifo[SVGA_FIFO_MAX] != g_svga.fifo_size)
		return "the FIFO bounds changed";
	next = fifo[SVGA_FIFO_NEXT_CMD];
	stop = fifo[SVGA_FIFO_STOP];
	if (next < g_svga.fifo_min || next >= g_svga.fifo_size ||
	    stop < g_svga.fifo_min || stop >= g_svga.fifo_size ||
	    ((next | stop) & 3))
		return "the FIFO pointers make no sense";
	return NULL;
}

// The device as probe set it up, for one that lost its state: what
// svga_fifo_init() and the probe register writes do, on the mapping and
// register layout probe chose.  Caller holds svga_modeset_mutex.  0, or
// -EIO when the device does not accept the FIFO.
static int svga_pm_reinit(uint32_t fence_seed)
{
	uint32_t enable, min, max, fifo_caps, seed;
	uint64_t f;

	// The FIFO stopped while it is laid out again: a host still reading
	// it would otherwise see NEXT_CMD and STOP move one at a time.
	if (svga_read_reg(SVGA_REG_CONFIG_DONE) != 0)
		svga_write_reg(SVGA_REG_CONFIG_DONE, 0);

	// The FIFO runs only on an enabled device.  As it was at suspend --
	// shown, or hidden behind a display manager's screens -- and enabled
	// but hidden when it was off then: whoever had it off (a blanked
	// console, see the mode restore) turns it off again.
	enable = g_svga_pm.enable;
	if (!(enable & SVGA_REG_ENABLE_ENABLE))
		enable = SVGA_REG_ENABLE_ENABLE | SVGA_REG_ENABLE_HIDE;
	svga_write_reg(SVGA_REG_ENABLE, enable);
	svga_write_reg(SVGA_REG_TRACES, g_svga_pm.traces);

	if (g_svga.caps & SVGA_CAP_EXTENDED_FIFO) {
		uint32_t regs = svga_read_reg(SVGA_REG_MEM_REGS);

		if (regs != g_svga.fifo_num_regs)
			kprintf("svga2: resume: FIFO register count %u, was %u "
				"(layout kept)\n",
				regs, g_svga.fifo_num_regs);
	}
	svga_fifo_reset_ring();
	svga_write_reg(SVGA_REG_CONFIG_DONE, 1);

	max = g_svga.fifo[SVGA_FIFO_MAX];
	min = g_svga.fifo[SVGA_FIFO_MIN];
	if (min >= max) {
		kprintf("svga2: resume: FIFO memory is not usable (min 0x%x "
			"max 0x%x)\n",
			min, max);
		return -EIO;
	}
	fifo_caps = (g_svga.caps & SVGA_CAP_EXTENDED_FIFO) ?
			    g_svga.fifo[SVGA_FIFO_CAPABILITIES] :
			    0;
	if (fifo_caps != g_svga.fifo_caps) {
		kprintf("svga2: resume: FIFO capabilities 0x%x, were 0x%x\n",
			fifo_caps, g_svga.fifo_caps);
		g_svga.fifo_caps = fifo_caps;
	}
	svga_write_reg(SVGA_REG_GUEST_ID, SVGA_GUEST_ID_OTHER);

	// The fence register as the last value handed out: everything issued
	// before the suspend reads as passed (it was drained, or went with
	// the reset), and the next value continues the sequence.  The counter
	// moves under the FIFO lock, as every allocation does.
	spin_lock_irqsave(&svga_fifo_lock, &f);
	seed = g_svga.next_fence;
	if (svga_fence_after(fence_seed, seed))
		seed = fence_seed;
	g_svga.next_fence = seed;
	if (svga_has_fence())
		g_svga.fifo[SVGA_FIFO_FENCE] = seed;
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	return 0;
}

// Does the console scan out of video memory through this driver right now:
// it holds the mode, no display-manager master has the display, and its
// screen updates still come here (a console that became a client of the
// display manager installed a flush hook of its own)?
static int svga_console_owns_display(void)
{
	return g_svga.active && !vmsvga2_hw_display_taken() &&
	       fb_get_flush_hook() == vmsvga2_update_rect;
}

int vmsvga2_hw_suspend(bool reset)
{
	if (!g_svga.present)
		return -ENODEV;
	if (!sched_current() || irqs_disabled())
		return -EINVAL;
	if (reset && g_cmd_channel) {
		kprintf("svga2: suspend with reset refused: the command "
			"buffers' owner still has its channel registered\n");
		return -EBUSY;
	}

	svga_mutex_lock(&svga_modeset_mutex);
	if (g_svga.pm_suspended) {
		svga_mutex_unlock(&svga_modeset_mutex);
		return -EBUSY;
	}
	svga_pm_quiesce();
	g_svga_pm.enable = svga_read_reg(SVGA_REG_ENABLE);
	g_svga_pm.traces = svga_read_reg(SVGA_REG_TRACES);
	g_svga_pm.crtc_enabled = g_svga.crtc.enabled;
	if (reset) {
		svga_device_release(SVGA_REG_ENABLE_DISABLE);
		// No DMA from a device that is not running.
		svga_pci_command_update(0, SVGA_PCI_CMD_MASTER);
		g_svga_pm.state = SVGA_PM_RESET;
	} else {
		g_svga_pm.state = SVGA_PM_LIGHT;
	}
	svga_mutex_unlock(&svga_modeset_mutex);

	kprintf("svga2: suspended (%s), fence 0x%x\n",
		reset ? "device reset" : "device left running",
		g_svga_pm.fence);
	return 0;
}

int vmsvga2_hw_resume(uint32_t fence_seed)
{
	uint32_t version = g_svga.version, caps, mem_start, mem_size;
	uint32_t legacy = 0, gmr2 = 0;
	const char *lost = NULL;
	int reinit, console = 0, rc = 0;
	uint64_t f;

	if (!g_svga.present)
		return -ENODEV;
	if (!sched_current() || irqs_disabled())
		return -EINVAL;

	svga_mutex_lock(&svga_modeset_mutex);
	if (!g_svga.pm_suspended) {
		svga_mutex_unlock(&svga_modeset_mutex);
		return 0;
	}
	if (g_svga_pm.state == SVGA_PM_SHUTDOWN) {
		svga_mutex_unlock(&svga_modeset_mutex);
		return -ENODEV;
	}

	svga_pci_command_update(SVGA_PCI_CMD_IO | SVGA_PCI_CMD_MEMORY |
					SVGA_PCI_CMD_MASTER,
				0);
	if (svga_negotiate_version() != 0) {
		svga_mutex_unlock(&svga_modeset_mutex);
		kprintf("svga2: resume: version negotiation failed\n");
		return -EIO;
	}
	if (g_svga.version != version)
		kprintf("svga2: resume: device version %u, was %u\n",
			g_svga.version, version);

	// The capabilities everything above was configured for stay in force;
	// a host that changed them is reported.
	caps = svga_read_reg(SVGA_REG_CAPABILITIES);
	if (caps != g_svga.caps)
		kprintf("svga2: resume: capabilities 0x%x, were 0x%x (kept)\n",
			caps, g_svga.caps);
	if (g_svga.caps & SVGA_CAP_CAP2_REGISTER) {
		uint32_t caps2 = svga_read_reg(SVGA_REG_CAP2);

		if (caps2 != g_svga_info.caps2)
			kprintf("svga2: resume: capabilities2 0x%x, were 0x%x "
				"(kept)\n",
				caps2, g_svga_info.caps2);
	}
	// The FIFO stays where it was mapped, or the device is not usable.
	mem_start = svga_read_reg(SVGA_REG_MEM_START);
	mem_size = svga_read_reg(SVGA_REG_MEM_SIZE);
	if (mem_start != (uint32_t)g_svga.fifo_phys ||
	    mem_size != g_svga.fifo_size) {
		svga_mutex_unlock(&svga_modeset_mutex);
		kprintf("svga2: resume: FIFO moved (0x%x+%u, was 0x%lx+%u)\n",
			mem_start, mem_size, (unsigned long)g_svga.fifo_phys,
			g_svga.fifo_size);
		return -EIO;
	}

	reinit = g_svga_pm.state == SVGA_PM_RESET;
	if (!reinit) {
		lost = svga_pm_state_lost();
		reinit = lost != NULL;
	}
	if (reinit) {
		rc = svga_pm_reinit(fence_seed);
		if (rc) {
			svga_mutex_unlock(&svga_modeset_mutex);
			return rc;
		}
	} else {
		// Kept its state: the fence register is the device's own.
		// Only the counter is not to fall behind the caller's.
		spin_lock_irqsave(&svga_fifo_lock, &f);
		if (svga_fence_after(fence_seed, g_svga.next_fence))
			g_svga.next_fence = fence_seed;
		spin_unlock_irqrestore(&svga_fifo_lock, f);
	}

	svga_irq_reinstall();
	g_svga_pm.state = SVGA_PM_RUNNING;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	g_svga.pm_suspended = 0;

	if (reinit)
		legacy = svga_gmr_replay(&gmr2);

	// The console's mode, when the console is what is on the screen; a
	// display manager's modes are its own resume's to set.
	console = svga_console_owns_display() && g_svga.crtc.width &&
		  g_svga.crtc.height;
	if (console && reinit) {
		if (svga_program_mode(g_svga.crtc.width, g_svga.crtc.height,
				      g_svga.crtc.bpp, 0) != 0)
			console = 0;
	}
	if (console && reinit && !g_svga_pm.crtc_enabled) {
		// It was blanked: blanked again, the way it was blanked.
#if VMSVGA2_ENABLE_TOGGLE
		svga_write_reg(SVGA_REG_ENABLE, 0);
#else
		svga_scanout_disable(svga_blank_hides());
#endif
		g_svga.crtc.enabled = 0;
	}
	svga_mutex_unlock(&svga_modeset_mutex);

	svga_cursor_resume(reinit);
	if (console && g_svga.crtc.enabled)
		vmsvga2_update_full();
	(void)svga_console_flush_pending();
	svga_report_errors();

	if (!reinit)
		kprintf("svga2: resumed, device state kept\n");
	else if (lost)
		kprintf("svga2: resumed, device re-initialized (%s); %u "
			"descriptor regions programmed, %u GMR2 regions for "
			"their owner, %s\n",
			lost, legacy, gmr2,
			console ? "console mode set" : "display left to its owner");
	else
		kprintf("svga2: resumed, device re-initialized; %u descriptor "
			"regions programmed, %u GMR2 regions for their owner, "
			"%s\n",
			legacy, gmr2,
			console ? "console mode set" : "display left to its owner");
	return reinit ? 1 : 0;
}

int vmsvga2_hw_suspended(void)
{
	return g_svga.pm_suspended ? 1 : 0;
}

// Hand the device back as it was found: the suspend's quiesce (everything
// queued drained, the FIFO closed to further work, the interrupt masked and
// let go), then SVGA_REG_CONFIG_DONE, _ENABLE and _TRACES get the values they
// had at probe.  CONFIG_DONE going back stops the FIFO, so this is the last
// thing done with the device; a display manager driving it through command
// buffers has to have let go of it first.  A device already suspended is
// quiesced already and only handed back.
void vmsvga2_shutdown(void)
{
	if (!g_svga.present)
		return;
	WARN_ON_ONCE(g_cmd_channel != NULL);
	svga_mutex_lock(&svga_modeset_mutex);
	if (!g_svga.pm_suspended)
		svga_pm_quiesce();
	g_svga.active = 0;
	g_svga.crtc.enabled = 0;
	svga_device_release(g_svga_info.saved_enable);
	g_svga_pm.state = SVGA_PM_SHUTDOWN;
	svga_mutex_unlock(&svga_modeset_mutex);
}

#else /* !VMSVGA2_PM */

int vmsvga2_hw_suspend(bool reset)
{
	(void)reset;
	return -EOPNOTSUPP;
}

int vmsvga2_hw_resume(uint32_t fence_seed)
{
	(void)fence_seed;
	return -EOPNOTSUPP;
}

int vmsvga2_hw_suspended(void)
{
	return 0;
}

// Hand the device back as it was found.  Everything queued is drained (a
// legacy sync on top: SVGA_REG_SYNC until SVGA_REG_BUSY clears), the
// interrupt is masked, and SVGA_REG_CONFIG_DONE, _ENABLE and _TRACES get the
// values they had at probe.  CONFIG_DONE going back to 0 stops the FIFO, so
// this is the last thing done with the device; a display manager driving it
// through command buffers has to have let go of it first.
void vmsvga2_shutdown(void)
{
	if (!g_svga.present)
		return;
	WARN_ON_ONCE(g_cmd_channel != NULL);
	vmsvga2_fifo_flush();
	(void)svga_legacy_sync();
	svga_irq_uninstall();
	g_svga.active = 0;
	g_svga.crtc.enabled = 0;
	svga_device_release(g_svga_info.saved_enable);
}

#endif /* VMSVGA2_PM */
