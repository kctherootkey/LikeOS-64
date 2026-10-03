// LikeOS VMware SVGA II display driver -- register access, and the hardware
// exports for the display-manager (DRM-style) driver in kernel/dev/gpu.
//
// That driver owns the device's 3D/KMS interface to userspace; this driver
// keeps owning the hardware access primitives (register file, FIFO, fence
// IRQ) and the boot-time console framebuffer.  The two meet here (and in
// the FIFO, interrupt and cursor exports of vmsvga2_fifo.c, vmsvga2_irq.c
// and vmsvga2_cursor.c).
//
// Also here: the register sequences of a legacy mode set -- pitch lock, the
// mode registers and the colour-depth check, the single-display topology, and
// SVGA_REG_ENABLE with its HIDE bit -- which the console's mode setting in
// vmsvga2.c is built from.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright (c) 2009-2024 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
// Portions Copyright 2009-2023 VMware, Inc., Palo Alto, CA., USA

#include "vmsvga2_priv.h"
#include <kernel/ke/syscall.h> /* errno values */

// Write the single-display topology (SVGA_REG_NUM_GUEST_DISPLAYS and the
// SVGA_REG_DISPLAY_* block) after every legacy mode set on a device with
// SVGA_CAP_DISPLAY_TOPOLOGY.  0 leaves those registers to the host, as this
// driver always did before.
#ifndef VMSVGA2_WRITE_TOPOLOGY
#define VMSVGA2_WRITE_TOPOLOGY 1
#endif

// Whether the display manager's mode sets (vmsvga2_hw_set_mode and
// vmsvga2_hw_set_mode_device) also reset the device by cycling
// SVGA_REG_ENABLE through 0.  They do not: a reset there would throw away
// the command-buffer contexts of a device that is running them.  The one
// caller that needs the reset -- the display manager's fallback from a
// refused screen target, which must drop the screen target the host may
// still be showing -- asks for it itself (vmsvga2_hw_svga_disable(false)
// right before its mode set, in vmw_display_fallback()).  1 restores the
// reset on every one of these mode sets.  The console's own mode sets do
// not reset the device either -- see VMSVGA2_ENABLE_TOGGLE in vmsvga2.c.
#ifndef VMSVGA2_HW_SET_MODE_RESETS
#define VMSVGA2_HW_SET_MODE_RESETS 0
#endif

// ===========================================================================
// Register access (index/value port pair, serialized by svga_reg_lock)
// ===========================================================================

spinlock_t svga_reg_lock = SPINLOCK_INIT("svga_reg");

uint32_t svga_read_reg(uint32_t index)
{
	uint64_t f;
	uint32_t val;

	spin_lock_irqsave(&svga_reg_lock, &f);
	outl(g_svga.io_base + SVGA_INDEX_PORT, index);
	val = inl(g_svga.io_base + SVGA_VALUE_PORT);
	spin_unlock_irqrestore(&svga_reg_lock, f);
	return val;
}

void svga_write_reg(uint32_t index, uint32_t value)
{
	uint64_t f;

	spin_lock_irqsave(&svga_reg_lock, &f);
	outl(g_svga.io_base + SVGA_INDEX_PORT, index);
	outl(g_svga.io_base + SVGA_VALUE_PORT, value);
	spin_unlock_irqrestore(&svga_reg_lock, f);
}

// ===========================================================================
// Legacy mode-set register sequences
// ===========================================================================

// The line pitch the device is locked to, from whichever of the two lock
// registers a mode set would write; 0 when the device has neither.  The
// FIFO one exists only with the extended FIFO, when the FIFO capabilities
// name it and the declared register block reaches it
// (svga_fifo_have_pitchlock()).
uint32_t svga_read_pitchlock(void)
{
	if (g_svga.caps & SVGA_CAP_PITCHLOCK)
		return svga_read_reg(SVGA_REG_PITCHLOCK);
	if (svga_fifo_have_pitchlock())
		return g_svga.fifo[SVGA_FIFO_PITCHLOCK];
	return 0;
}

// Lock the scan-out line pitch: the register when the device has one, the
// FIFO register otherwise, nothing on a device with neither (its pitch is
// then width times bytes per pixel, which is what every caller asks for).
void svga_write_pitchlock(uint32_t pitch)
{
	if (g_svga.caps & SVGA_CAP_PITCHLOCK)
		svga_write_reg(SVGA_REG_PITCHLOCK, pitch);
	else if (svga_fifo_have_pitchlock())
		g_svga.fifo[SVGA_FIFO_PITCHLOCK] = pitch;
}

// The colour depth a scan-out of `bpp' bits per pixel has in the formats
// the console draws: XRGB8888 (24) and RGB565 (16).  0 for anything else.
uint32_t svga_format_depth(uint32_t bpp)
{
	if (bpp == 32)
		return 24;
	if (bpp == 16)
		return 16;
	return 0;
}

// Program the mode registers for one legacy scan-out of `width' x `height'
// with lines of `pitch' bytes, `bpp' bits per pixel and colour depth
// `depth'.
//
// The pitch is locked first, so the device lays the lines out the way the
// guest will draw them.  The pixel size is written only when the device
// emulates guest pixel sizes (SVGA_CAP_8BIT_EMULATION); without that the
// guest runs at the host's size and the register is the host's to report.
// Either way the device then has to agree on the colour depth: a scan-out
// the device would interpret with a different layout is refused, -EINVAL,
// leaving the registers written -- the caller restores them.
//
// A depth of 32 at 32 bpp is taken for the 24 it means: some hosts report
// the pixel size there, and the alpha byte of an XRGB8888 scan-out is not
// shown either way.
int svga_kms_write_svga(uint32_t width, uint32_t height, uint32_t pitch,
			uint32_t bpp, uint32_t depth)
{
	uint32_t host_depth;

	svga_write_pitchlock(pitch);
	svga_write_reg(SVGA_REG_WIDTH, width);
	svga_write_reg(SVGA_REG_HEIGHT, height);
	if (g_svga.caps & SVGA_CAP_8BIT_EMULATION)
		svga_write_reg(SVGA_REG_BITS_PER_PIXEL, bpp);

	host_depth = svga_read_reg(SVGA_REG_DEPTH);
	if (host_depth == depth || (bpp == 32 && depth == 24 && host_depth == 32))
		return 0;
	kprintf("svga2: invalid depth %u for %u bpp, host expects %u\n", depth,
		bpp, host_depth);
	return -EINVAL;
}

// Tell the host the guest's display layout: one display, primary, at the
// origin, `width' x `height'.  Without SVGA_CAP_DISPLAY_TOPOLOGY the host
// takes the layout to be the mode registers, so there is nothing to write.
void svga_ldu_commit_topology(uint32_t width, uint32_t height)
{
#if VMSVGA2_WRITE_TOPOLOGY
	if (!(g_svga.caps & SVGA_CAP_DISPLAY_TOPOLOGY))
		return;
	// "Always show something": at least one display, however many the
	// guest drives.
	svga_write_reg(SVGA_REG_NUM_GUEST_DISPLAYS, 1);
	svga_write_reg(SVGA_REG_DISPLAY_ID, 0);
	svga_write_reg(SVGA_REG_DISPLAY_IS_PRIMARY, 1);
	svga_write_reg(SVGA_REG_DISPLAY_POSITION_X, 0);
	svga_write_reg(SVGA_REG_DISPLAY_POSITION_Y, 0);
	svga_write_reg(SVGA_REG_DISPLAY_WIDTH, width);
	svga_write_reg(SVGA_REG_DISPLAY_HEIGHT, height);
#else
	(void)width;
	(void)height;
#endif
}

// Legacy scan-out on: SVGA_REG_ENABLE_ENABLE, written only when the device
// is not already in exactly that state (enabled, not hidden).  Never cycles
// the register through 0: disabling the device resets it, and a reset stops
// the command-buffer contexts and drops the guest-backed objects a display
// manager may be using.
void svga_scanout_enable(void)
{
	if (svga_read_reg(SVGA_REG_ENABLE) != SVGA_REG_ENABLE_ENABLE)
		svga_write_reg(SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE);
}

// Legacy scan-out off.  hide: the device stays enabled -- FIFO, command
// buffers, guest-backed objects and screens keep working -- and only the
// legacy framebuffer is no longer shown.  !hide: SVGA_REG_ENABLE_DISABLE, the
// device falls back to its VGA personality, which resets it.
void svga_scanout_disable(int hide)
{
	svga_write_reg(SVGA_REG_ENABLE,
		       hide ? (SVGA_REG_ENABLE_ENABLE | SVGA_REG_ENABLE_HIDE) :
			      SVGA_REG_ENABLE_DISABLE);
}

// ===========================================================================
// Exports for the display-manager driver
// ===========================================================================

int vmsvga2_hw_present(void)
{
	return g_svga.present;
}

const pci_device_t *vmsvga2_hw_pci(void)
{
	return g_svga.present ? g_svga.pci : NULL;
}

uint32_t vmsvga2_hw_read_reg(uint32_t index)
{
	return svga_read_reg(index);
}

void vmsvga2_hw_write_reg(uint32_t index, uint32_t value)
{
	svga_write_reg(index, value);
}

int vmsvga2_hw_has_fifo_cap(uint32_t cap)
{
	return svga_has_fifo_cap(cap);
}

// FIFO registers for the display-manager driver: present only when the
// device declared them (svga_has_fifo_reg()), so a register the device does
// not have reads 0 here -- never the padding the page-sized register area
// leaves between the last declared register and the ring.
int vmsvga2_hw_has_fifo_reg(uint32_t reg)
{
	return svga_has_fifo_reg(reg);
}

uint32_t vmsvga2_hw_fifo_reg(uint32_t reg)
{
	return svga_has_fifo_reg(reg) ? g_svga.fifo[reg] : 0;
}

void vmsvga2_hw_geometry(struct vmsvga2_hw_geometry *g)
{
	g->fb_phys = g_svga.fb_phys;
	g->fb_virt = g_svga.fb_virt;
	g->vram_size = g_svga.vram_size;
	g->max_width = g_svga.max_width;
	g->max_height = g_svga.max_height;
	g->width = g_svga.crtc.width;
	g->height = g_svga.crtc.height;
	g->bpp = g_svga.crtc.bpp;
	g->pitch = g_svga.crtc.pitch;
	g->fb_offset = g_svga.crtc.fb_offset;
	g->caps = g_svga.caps;
	g->fifo_caps = g_svga.fifo_caps;
	g->fifo_size = g_svga.fifo_size;
	g->irq_enabled = g_svga.irq_enabled;
}

/* Display ownership hand-over, the part of it that is this device's: the
 * generic side -- the console neither painting nor listening while a
 * master holds the screen, and its full redraw on release -- is done by
 * drm_master_set()/drm_master_drop() for every driver (fbdev_opened /
 * fbdev_closed), so it is not repeated here. */
static int g_hw_display_taken;

void vmsvga2_hw_display_take(void)
{
	if (g_hw_display_taken)
		return;
	g_hw_display_taken = 1;
}

void vmsvga2_hw_display_release(void)
{
	if (!g_hw_display_taken)
		return;
	g_hw_display_taken = 0;
	svga_write_reg(SVGA_REG_TRACES, 1);
}

int vmsvga2_hw_display_taken(void)
{
	return g_hw_display_taken;
}

/* Mode set for the display-manager, device only.
 *
 * The console does NOT follow.  For a console that is itself a client of
 * the display manager (drm_console.c) its framebuffer is a buffer object,
 * not this device's video memory, and cascading a client's mode through
 * console_reinit_framebuffer() would move the console back onto video
 * memory that nothing is scanning out -- in the middle of somebody else's
 * session, at that. */
int vmsvga2_hw_set_mode_device(uint32_t width, uint32_t height)
{
	return svga_set_mode(width, height, 32, VMSVGA2_HW_SET_MODE_RESETS);
}

/* Mode set for the display-manager: the console's framebuffer geometry
 * follows (console_reinit_framebuffer), as the fbdev ioctl path does. */
int vmsvga2_hw_set_mode(uint32_t width, uint32_t height)
{
	framebuffer_info_t fi;
	int rc = svga_set_mode(width, height, 32, VMSVGA2_HW_SET_MODE_RESETS);

	if (rc != 0)
		return rc;
	if (vmsvga2_get_info(&fi) != 0)
		return -1;
	/* Cascade the new geometry through fb/console/tty (SIGWINCH). */
	if (console_reinit_framebuffer(&fi) != 0)
		return -1;
	return 0;
}
