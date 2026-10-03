// LikeOS VMware SVGA II display driver -- the hardware cursor.
//
// The device composites a cursor of its own over the scanout: an image
// defined by a command (1 bpp AND/XOR masks, or 32-bit ARGB on hosts with
// SVGA_CAP_ALPHA_CURSOR) and a position, which takes one of three ways to the
// host, best first:
//
//   CURSOR4  the SVGA_REG_CURSOR4_* register block (SVGA_CAP2_EXTRA_REGS):
//            x, y, screen, on, then a write to SUBMIT publishes the four
//            together, so the host never sees half an update;
//   BYPASS3  the FIFO cursor registers (SVGA_FIFO_CAP_CURSOR_BYPASS_3, and
//            only while the FIFO's register block reaches CURSOR_COUNT): x,
//            y, on, then CURSOR_COUNT is bumped -- the host takes a new
//            count as "the registers changed";
//   LEGACY   the original SVGA_REG_CURSOR_* registers, one VM exit each.
//
// cursor_lock protects the cached image, the cached position and the
// position registers; it is never held while a define is handed to the host
// (see svga_cursor_push()).  The image is cached so a device reset can
// define it again (svga_cursor_restore()).
//
// Where a define goes.  Once the display-manager driver owns command
// buffers (vmsvga2_set_cmd_channel()), the device's command stream is that
// channel, and a define from a context that may sleep goes down it -- in
// order with the screen updates around it.  A define from a context that may
// not (the mouse interrupt drives the console's pointer) goes into the FIFO,
// as it always has; the channel's submit can wait for a free buffer, which an
// interrupt handler must not.  Defines are sent one at a time and always as
// the newest cached image: a define arriving while another is on its way to
// the channel only replaces the cache, and the sender sends again before it
// lets go -- so the image the host ends up with is the last one asked for,
// whichever way each request went.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2024-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include "vmsvga2_priv.h"
#include <kernel/ke/syscall.h> /* errno values */

// Position through the CURSOR4 register block when the host offers it.  Off
// falls back to the FIFO bypass / legacy registers, the only paths before.
#ifndef VMSVGA2_CURSOR4
#define VMSVGA2_CURSOR4 1
#endif

static spinlock_t svga_cursor_lock = SPINLOCK_INIT("svga_cursor");

#define SVGA_CURSOR_MAX_DIM 64
#define SVGA_CURSOR_ID 0

// A DEFINE_ALPHA_CURSOR command: the command word, then the header, then
// width * height ARGB pixels.
struct svga_cmd_define_alpha_cursor {
	uint32_t cmd;
	SVGAFifoCmdDefineAlphaCursor cursor;
};

// ...and a DEFINE_CURSOR: the header, then the AND mask, then the XOR mask.
struct svga_cmd_define_cursor {
	uint32_t cmd;
	SVGAFifoCmdDefineCursor cursor;
};

// The largest define command: a 64x64 alpha image.
#define SVGA_CURSOR_CMD_DWORDS                                               \
	(sizeof(struct svga_cmd_define_alpha_cursor) / sizeof(uint32_t) +   \
	 SVGA_CURSOR_MAX_DIM * SVGA_CURSOR_MAX_DIM)

static struct {
	int valid;
	int alpha; // cached image is ARGB (vs 1-bit AND/XOR)
	uint32_t width, height, hot_x, hot_y;
	uint32_t argb[SVGA_CURSOR_MAX_DIM * SVGA_CURSOR_MAX_DIM];
	uint32_t and_mask[SVGA_CURSOR_MAX_DIM * 2]; // 1bpp rows, dword-padded
	uint32_t xor_mask[SVGA_CURSOR_MAX_DIM * 2];

	// Bumped with every new image; sent_gen is the image the host was
	// last given.  Both under svga_cursor_lock.
	uint32_t gen;
	uint32_t sent_gen;
	// A sender is handing the cache to the host (lock dropped while it
	// does); see svga_cursor_push().
	int send_busy;

	// Last position written, so a visibility change on the CURSOR4 path
	// (which always publishes x, y and on together) keeps the cursor where
	// it was.
	int32_t x, y;
	// ...and the visibility last written with it (SVGA_CURSOR_ON_*), and
	// whether anything was written at all: what a resume puts back.
	uint32_t on;
	int pos_valid;
} g_cursor;

// The define command being sent, owned by whoever set g_cursor.send_busy.
static uint32_t g_cursor_cmd[SVGA_CURSOR_CMD_DWORDS];

// ===========================================================================
// Which position registers
// ===========================================================================

// The FIFO bypass needs more than the capability bit: the host watches
// CURSOR_COUNT, and a FIFO whose register block stops short of it has
// nowhere to put the count.
static int svga_cursor_bypass3_enabled(void)
{
	return svga_has_fifo_cap(SVGA_FIFO_CAP_CURSOR_BYPASS_3) &&
	       svga_has_fifo_reg(SVGA_FIFO_CURSOR_COUNT);
}

static int svga_cursor_path(void)
{
	if (!g_svga.present)
		return VMSVGA2_CURSOR_PATH_NONE;
#if VMSVGA2_CURSOR4
	// SVGA_REG_CAP2 as read at probe: constant while the device is up.
	if (svga_device_caps2() & SVGA_CAP2_EXTRA_REGS)
		return VMSVGA2_CURSOR_PATH_CURSOR4;
#endif
	if (svga_cursor_bypass3_enabled())
		return VMSVGA2_CURSOR_PATH_BYPASS3;
	if (g_svga.caps & (SVGA_CAP_CURSOR_BYPASS | SVGA_CAP_CURSOR_BYPASS_2))
		return VMSVGA2_CURSOR_PATH_LEGACY;
	return VMSVGA2_CURSOR_PATH_NONE;
}

// Probe-time: say which path the position takes.
void svga_cursor_init(void)
{
	static const char *const names[] = { "none", "CURSOR4 registers",
					     "FIFO bypass 3",
					     "legacy registers" };
	int path;

	path = svga_cursor_path();
	if (!(g_svga.caps & (SVGA_CAP_CURSOR | SVGA_CAP_ALPHA_CURSOR)))
		return;
	kprintf("svga2: hardware cursor: %s image, position through %s\n",
		(g_svga.caps & SVGA_CAP_ALPHA_CURSOR) ? "alpha" : "mono",
		names[path]);
}

int vmsvga2_has_hw_cursor(void)
{
	if (!g_svga.present)
		return 0;
	if (!(g_svga.caps & (SVGA_CAP_CURSOR | SVGA_CAP_ALPHA_CURSOR)))
		return 0;
	// Position updates need one of the position mechanisms.
	return svga_cursor_path() != VMSVGA2_CURSOR_PATH_NONE;
}

// ===========================================================================
// Position
// ===========================================================================

// Write position and visibility through `path'; caller holds
// svga_cursor_lock.  `on' is SVGA_CURSOR_ON_SHOW or SVGA_CURSOR_ON_HIDE.
static void svga_cursor_write_position(int path, int32_t x, int32_t y,
				       uint32_t on)
{
	switch (path) {
	case VMSVGA2_CURSOR_PATH_CURSOR4:
		svga_write_reg(SVGA_REG_CURSOR4_X, (uint32_t)x);
		svga_write_reg(SVGA_REG_CURSOR4_Y, (uint32_t)y);
		svga_write_reg(SVGA_REG_CURSOR4_SCREEN_ID,
			       SVGA_ID_INVALID_SCREEN);
		svga_write_reg(SVGA_REG_CURSOR4_ON, on);
		svga_write_reg(SVGA_REG_CURSOR4_SUBMIT, 1);
		break;
	case VMSVGA2_CURSOR_PATH_BYPASS3:
		// Lowest-latency FIFO path: memory writes, no command.
		g_svga.fifo[SVGA_FIFO_CURSOR_ON] = on;
		g_svga.fifo[SVGA_FIFO_CURSOR_X] = (uint32_t)x;
		g_svga.fifo[SVGA_FIFO_CURSOR_Y] = (uint32_t)y;
		if (svga_has_fifo_reg(SVGA_FIFO_CURSOR_SCREEN_ID))
			g_svga.fifo[SVGA_FIFO_CURSOR_SCREEN_ID] =
				SVGA_ID_INVALID_SCREEN;
		// The count last: it is what tells the host to look.
		__asm__ volatile("" ::: "memory");
		g_svga.fifo[SVGA_FIFO_CURSOR_COUNT]++;
		break;
	case VMSVGA2_CURSOR_PATH_LEGACY:
		svga_write_reg(VMSVGA2_REG_CURSOR_ID, SVGA_CURSOR_ID);
		svga_write_reg(SVGA_REG_CURSOR_X, (uint32_t)x);
		svga_write_reg(SVGA_REG_CURSOR_Y, (uint32_t)y);
		svga_write_reg(SVGA_REG_CURSOR_ON, on);
		break;
	default:
		break;
	}
	g_cursor.x = x;
	g_cursor.y = y;
	g_cursor.on = on;
	g_cursor.pos_valid = 1;
}

/* A hide through the FIFO bypass also goes through the legacy register.
 *
 * The two are meant to be the same control, but they are implemented
 * separately, and a host that honours only one of them leaves the cursor on
 * screen after the other has been told to take it away.  Getting that wrong
 * is visible -- a second pointer beside the one the program that owns the
 * display is drawing -- so the hide is asserted both ways.  Showing is not:
 * exactly one path should own the position updates, and svga_cursor_path()
 * decides which.
 *
 * Not on the CURSOR4 path.  That register block replaces the bypass
 * registers on a host that offers it, and its hide is published whole with
 * SUBMIT; the legacy register is not written there at all.  A legacy hide
 * written beside it is a second, older control the CURSOR4 show never
 * clears: on VMware the pointer of a display server stayed invisible --
 * clicks landed, nothing was drawn -- after the console had hidden its own
 * pointer that way at the hand-over.  Caller holds svga_cursor_lock. */
static void svga_cursor_assert_hidden(int path)
{
	if (path == VMSVGA2_CURSOR_PATH_BYPASS3)
		svga_write_reg(SVGA_REG_CURSOR_ON, SVGA_CURSOR_ON_HIDE);
}

int vmsvga2_cursor_move(int32_t x, int32_t y, int visible)
{
	uint64_t f;
	int path;

	if (!g_svga.active)
		return -1;
	if (!vmsvga2_has_hw_cursor())
		return -1;
	path = svga_cursor_path();

	spin_lock_irqsave(&svga_cursor_lock, &f);
	svga_cursor_write_position(path, x, y,
				   visible ? SVGA_CURSOR_ON_SHOW :
					     SVGA_CURSOR_ON_HIDE);
	if (!visible)
		svga_cursor_assert_hidden(path);
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return 0;
}

int vmsvga2_cursor_show(int visible)
{
	uint32_t on = visible ? SVGA_CURSOR_ON_SHOW : SVGA_CURSOR_ON_HIDE;
	uint64_t f;
	int path;

	if (!g_svga.active || !vmsvga2_has_hw_cursor())
		return -1;
	path = svga_cursor_path();

	spin_lock_irqsave(&svga_cursor_lock, &f);
	switch (path) {
	case VMSVGA2_CURSOR_PATH_CURSOR4:
		// The block is published whole: keep the last position.
		svga_cursor_write_position(path, g_cursor.x, g_cursor.y, on);
		break;
	case VMSVGA2_CURSOR_PATH_BYPASS3:
		g_svga.fifo[SVGA_FIFO_CURSOR_ON] = on;
		__asm__ volatile("" ::: "memory");
		g_svga.fifo[SVGA_FIFO_CURSOR_COUNT]++;
		break;
	default:
		svga_write_reg(SVGA_REG_CURSOR_ON, on);
		break;
	}
	g_cursor.on = on;
	if (!visible)
		svga_cursor_assert_hidden(path);
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return 0;
}

// ===========================================================================
// Image
// ===========================================================================

// Build the define command for the cached image into `cmd'; returns its
// size in bytes.  Caller holds svga_cursor_lock and has checked .valid.
static uint32_t svga_cursor_build_define(uint32_t *cmd)
{
	if (g_cursor.alpha) {
		struct svga_cmd_define_alpha_cursor *c = (void *)cmd;
		uint32_t image_size =
			g_cursor.width * g_cursor.height * sizeof(uint32_t);

		kmemset(c, 0, sizeof(*c));
		c->cmd = SVGA_CMD_DEFINE_ALPHA_CURSOR;
		c->cursor.id = SVGA_CURSOR_ID;
		c->cursor.width = g_cursor.width;
		c->cursor.height = g_cursor.height;
		c->cursor.hotspotX = g_cursor.hot_x;
		c->cursor.hotspotY = g_cursor.hot_y;
		kmemcpy(&c[1], g_cursor.argb, image_size);
		return (uint32_t)sizeof(*c) + image_size;
	} else {
		struct svga_cmd_define_cursor *c = (void *)cmd;
		uint32_t row_dwords = (g_cursor.width + 31) / 32;
		uint32_t mask_bytes =
			row_dwords * g_cursor.height * sizeof(uint32_t);
		uint8_t *masks = (uint8_t *)&c[1];

		kmemset(c, 0, sizeof(*c));
		c->cmd = SVGA_CMD_DEFINE_CURSOR;
		c->cursor.id = SVGA_CURSOR_ID;
		c->cursor.hotspotX = g_cursor.hot_x;
		c->cursor.hotspotY = g_cursor.hot_y;
		c->cursor.width = g_cursor.width;
		c->cursor.height = g_cursor.height;
		c->cursor.andMaskDepth = 1;
		c->cursor.xorMaskDepth = 1;
		kmemcpy(masks, g_cursor.and_mask, mask_bytes);
		kmemcpy(masks + mask_bytes, g_cursor.xor_mask, mask_bytes);
		return (uint32_t)sizeof(*c) + 2 * mask_bytes;
	}
}

/* Hand the cached image to the host.
 *
 * `force' sends it even when the host already has this generation (after a
 * device reset the host has nothing).  `can_sleep' is whether the caller's
 * context may wait -- sampled by the caller before any lock was taken.
 * Returns 0 when the image was sent or left to the sender already at work
 * (which sends the newest cache before it finishes), -1 when sending failed
 * or there is nothing to send.
 *
 * One sender at a time: it builds the command under svga_cursor_lock and
 * sends it WITHOUT the lock -- down the command channel (whose submit may
 * wait for a buffer) from a context that may sleep, into the FIFO otherwise
 * (which may wait for room: sleeping where the caller may, a bounded spin
 * where it may not) -- and sends again for as long as a newer image arrived
 * meanwhile.  The position paths (an interrupt handler moving the pointer)
 * therefore never wait behind a define, and the FIFO is never entered with
 * this lock held.  Channel sends are flushed (ring = 1): a cursor shape is
 * wanted on screen now, not with the next batch. */
static int svga_cursor_push(int force, int can_sleep)
{
	int (*chan)(const void *cmds, uint32_t bytes, int ring) = g_cmd_channel;
	int use_chan = chan != NULL && can_sleep;
	uint64_t f;
	uint32_t bytes, gen;
	int rc = 0;

	spin_lock_irqsave(&svga_cursor_lock, &f);
	if (!g_cursor.valid) {
		spin_unlock_irqrestore(&svga_cursor_lock, f);
		return -1;
	}
	if (g_cursor.send_busy) {
		// The sender compares generations before it lets go.
		if (force)
			g_cursor.sent_gen = g_cursor.gen - 1;
		spin_unlock_irqrestore(&svga_cursor_lock, f);
		return 0;
	}
	if (!force && g_cursor.sent_gen == g_cursor.gen) {
		spin_unlock_irqrestore(&svga_cursor_lock, f);
		return 0;
	}

	g_cursor.send_busy = 1;
	for (;;) {
		gen = g_cursor.gen;
		bytes = svga_cursor_build_define(g_cursor_cmd);
		spin_unlock_irqrestore(&svga_cursor_lock, f);

		if (use_chan)
			rc = chan(g_cursor_cmd, bytes, 1);
		else
			rc = svga_fifo_write_cmd(g_cursor_cmd, bytes);

		spin_lock_irqsave(&svga_cursor_lock, &f);
		if (rc != 0)
			break;
		g_cursor.sent_gen = gen;
		if (!g_cursor.valid || g_cursor.sent_gen == g_cursor.gen)
			break;
	}
	g_cursor.send_busy = 0;
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return rc == 0 ? 0 : -1;
}

static int svga_cursor_context_can_sleep(void)
{
	return sched_current() != NULL && irqs_enabled();
}

// Define the cached image again after a device reset, if there is one.
void svga_cursor_restore(void)
{
	(void)svga_cursor_push(1, svga_cursor_context_can_sleep());
}

// After a suspend.  With `redefine' (the device was re-initialized and has
// no image) the image first, so the position never shows a cursor the host
// has no image for -- into the FIFO, the one stream certainly running right
// after a re-initialization: a command channel still registered belongs to
// contexts the reset took away, which their owner has yet to rebuild.  A
// device that kept its state kept the image too.  Then position and visibility as last written, through the path the device
// offers now -- after a reset the position registers (and the FIFO's bypass
// block, laid out again) hold nothing of it.  A hidden cursor is asserted
// hidden both ways, as every hide is.
void svga_cursor_resume(int redefine)
{
	uint64_t f;
	int path;

	if (redefine)
		(void)svga_cursor_push(1, 0);
	path = svga_cursor_path();
	if (path == VMSVGA2_CURSOR_PATH_NONE)
		return;
	spin_lock_irqsave(&svga_cursor_lock, &f);
	if (g_cursor.pos_valid) {
		svga_cursor_write_position(path, g_cursor.x, g_cursor.y,
					   g_cursor.on);
		if (g_cursor.on != SVGA_CURSOR_ON_SHOW)
			svga_cursor_assert_hidden(path);
	}
	spin_unlock_irqrestore(&svga_cursor_lock, f);
}

int vmsvga2_cursor_define(uint32_t width, uint32_t height, uint32_t hot_x,
			  uint32_t hot_y, const uint32_t *and_mask,
			  const uint32_t *xor_mask)
{
	int can_sleep = svga_cursor_context_can_sleep();
	uint64_t f;
	uint32_t mask_dwords;

	if (!g_svga.present || !(g_svga.caps & SVGA_CAP_CURSOR))
		return -1;
	if (!and_mask || !xor_mask)
		return -1;
	if (WARN_ON_ONCE(width == 0 || height == 0 ||
			 width > SVGA_CURSOR_MAX_DIM ||
			 height > SVGA_CURSOR_MAX_DIM))
		return -1;
	mask_dwords = ((width + 31) / 32) * height;

	spin_lock_irqsave(&svga_cursor_lock, &f);
	g_cursor.alpha = 0;
	g_cursor.width = width;
	g_cursor.height = height;
	g_cursor.hot_x = hot_x;
	g_cursor.hot_y = hot_y;
	kmemcpy(g_cursor.and_mask, and_mask, mask_dwords * sizeof(uint32_t));
	kmemcpy(g_cursor.xor_mask, xor_mask, mask_dwords * sizeof(uint32_t));
	g_cursor.valid = 1;
	g_cursor.gen++;
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return svga_cursor_push(0, can_sleep);
}

int vmsvga2_cursor_define_alpha(uint32_t width, uint32_t height,
				uint32_t hot_x, uint32_t hot_y,
				const uint32_t *argb_pixels)
{
	int can_sleep = svga_cursor_context_can_sleep();
	uint64_t f;

	if (!g_svga.present || !(g_svga.caps & SVGA_CAP_ALPHA_CURSOR))
		return -1;
	if (!argb_pixels)
		return -1;
	if (WARN_ON_ONCE(width == 0 || height == 0 ||
			 width > SVGA_CURSOR_MAX_DIM ||
			 height > SVGA_CURSOR_MAX_DIM))
		return -1;

	spin_lock_irqsave(&svga_cursor_lock, &f);
	g_cursor.alpha = 1;
	g_cursor.width = width;
	g_cursor.height = height;
	g_cursor.hot_x = hot_x;
	g_cursor.hot_y = hot_y;
	kmemcpy(g_cursor.argb, argb_pixels,
		(size_t)width * height * sizeof(uint32_t));
	g_cursor.valid = 1;
	g_cursor.gen++;
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return svga_cursor_push(0, can_sleep);
}

// ===========================================================================
// Exports for the display-manager driver (vmsvga2_hw.h): the same
// primitives with errno returns.
// ===========================================================================

int vmsvga2_hw_cursor_define_mono(uint32_t width, uint32_t height,
				  uint32_t hot_x, uint32_t hot_y,
				  const uint32_t *and_mask,
				  const uint32_t *xor_mask)
{
	if (!g_svga.present || !(g_svga.caps & SVGA_CAP_CURSOR))
		return -ENODEV;
	if (!and_mask || !xor_mask || width == 0 || height == 0 ||
	    width > SVGA_CURSOR_MAX_DIM || height > SVGA_CURSOR_MAX_DIM)
		return -EINVAL;
	return vmsvga2_cursor_define(width, height, hot_x, hot_y, and_mask,
				     xor_mask) == 0 ? 0 : -EIO;
}

int vmsvga2_hw_cursor_define_alpha(uint32_t width, uint32_t height,
				   uint32_t hot_x, uint32_t hot_y,
				   const uint32_t *argb_pixels)
{
	if (!g_svga.present || !(g_svga.caps & SVGA_CAP_ALPHA_CURSOR))
		return -ENODEV;
	if (!argb_pixels || width == 0 || height == 0 ||
	    width > SVGA_CURSOR_MAX_DIM || height > SVGA_CURSOR_MAX_DIM)
		return -EINVAL;
	return vmsvga2_cursor_define_alpha(width, height, hot_x, hot_y,
					   argb_pixels) == 0 ? 0 : -EIO;
}

int vmsvga2_hw_cursor_move(int32_t x, int32_t y, bool show)
{
	return vmsvga2_cursor_move(x, y, show ? 1 : 0) == 0 ? 0 : -ENODEV;
}

int vmsvga2_hw_cursor_show(void)
{
	return vmsvga2_cursor_show(1) == 0 ? 0 : -ENODEV;
}

int vmsvga2_hw_cursor_hide(void)
{
	return vmsvga2_cursor_show(0) == 0 ? 0 : -ENODEV;
}

int vmsvga2_hw_cursor_path(void)
{
	return svga_cursor_path();
}

/* Position and visibility for a driver that presents through screen objects
 * or targets: no requirement that the console owns the mode, and the
 * position registers are written on every call, shown or hidden. */
int vmsvga2_hw_cursor_update_position(bool show, int32_t x, int32_t y)
{
	uint64_t f;
	int path;

	if (!g_svga.present)
		return -ENODEV;
	path = svga_cursor_path();
	if (path == VMSVGA2_CURSOR_PATH_NONE)
		return -ENODEV;

	spin_lock_irqsave(&svga_cursor_lock, &f);
	svga_cursor_write_position(path, x, y,
				   show ? SVGA_CURSOR_ON_SHOW :
					  SVGA_CURSOR_ON_HIDE);
	if (!show)
		svga_cursor_assert_hidden(path);
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return 0;
}

/* The image now comes from somewhere else (a cursor MOB): a device reset
 * must not define the cached one over it. */
void vmsvga2_hw_cursor_forget(void)
{
	uint64_t f;

	spin_lock_irqsave(&svga_cursor_lock, &f);
	g_cursor.valid = 0;
	g_cursor.gen++;
	spin_unlock_irqrestore(&svga_cursor_lock, f);
}
