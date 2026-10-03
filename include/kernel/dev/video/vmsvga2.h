// LikeOS VMware SVGA II display driver
//
// The register file, FIFO layout, command numbers and capability bits of the
// SVGA II virtual display adapter (as implemented by VMware products, QEMU
// "-vga vmware" and VirtualBox "VMSVGA") come from the device headers in
// include/kernel/dev/gpu/vmwgfx/svga/, which the display-manager driver uses
// too, so both drivers name every register the same way and this header can
// be included beside the full device headers.  What remains here is this
// driver's own policy (which interrupt sources stay armed, timeouts, table
// sizes) and its API.
//
// Driver architecture (DRM/KMS-shaped):
//   svga_connector  -- the (virtual) monitor: display topology
//   svga_crtc       -- scanout state: current mode, enable/disable
//   svga_fb         -- a scanout buffer (the front buffer in VRAM)
//
// Source layout (kernel/dev/video/):
//   vmsvga2.c         probe, bring-up, mode setting, boot console, /dev/fb0
//                     backend, host-preferred resolution
//   vmsvga2_hw.c      register access and the exports for the display-manager
//                     driver (vmsvga2_hw.h)
//   vmsvga2_fifo.c    command FIFO, fences, drains
//   vmsvga2_irq.c     interrupt routing, the device interrupt, IRQ mask
//   vmsvga2_gmr.c     guest memory regions
//   vmsvga2_cursor.c  hardware cursor
//
// Locking model (every spinlock is taken with interrupts off):
//   fifo_lock (spin)    -- FIFO reserve..commit, producer index; never held
//                          while waiting for room (dropped, then retaken)
//   reg_lock  (spin)    -- index/value register port pairs
//   cursor_lock (spin)  -- cursor image cache + position registers; never
//                          held while a define is handed to the host
//   irq_lock  (spin)    -- IRQ-visible state (accumulated status)
//   irqmask_lock (spin) -- interrupt-source refcounts, the fence goal and
//                          SVGA_REG_IRQMASK
//   dirty_lock (spin)   -- the console's pending screen-update box
//   gmr_lock (spin)     -- guest memory region id table
//   modeset_mutex       -- sleeping mutex for mode changes (infrequent)
// Nesting: irqmask_lock -> reg_lock, cursor_lock -> reg_lock and (the
// resume's region replay) gmr_lock -> reg_lock only; reg_lock, irq_lock and
// dirty_lock are leaves.  fifo_lock nests
// nothing: the doorbell (reg_lock) is rung after dropping it (the legacy
// engine, VMSVGA2_LEGACY_FIFO, reads and rings registers under it).  The
// wait-queue locks of the fence/FIFO waits are never taken under any of
// these.  The modeset mutex is outermost (it may be held across register
// writes and prints, never taken under a spinlock).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_VIDEO_VMSVGA2_H_
#define _KERNEL_DEV_VIDEO_VMSVGA2_H_

#include <kernel/uapi/types.h>
#include <kernel/io/console.h> // framebuffer_info_t
#include <kernel/dev/gpu/vmwgfx/svga/svga_reg.h>

// ---------------------------------------------------------------------------
// PCI identity
// ---------------------------------------------------------------------------
#ifndef SVGA_PCI_VENDOR_ID
#define SVGA_PCI_VENDOR_ID 0x15AD
#endif
#ifndef SVGA_PCI_DEVICE_ID
#define SVGA_PCI_DEVICE_ID 0x0405
#endif

// The guest identifier written to SVGA_REG_GUEST_ID: a generic guest OS.
#ifndef SVGA_GUEST_ID_OTHER
#define SVGA_GUEST_ID_OTHER 0x500A
#endif

/* The sources that stay armed for the life of the device.
 *
 * A command buffer completes asynchronously, and a fence emitted through that
 * channel has passed when its buffer does -- so on any host new enough to use
 * command buffers this, and not ANY_FENCE, is the interrupt that says a fence
 * passed.  ANY_FENCE and FENCE_GOAL belong to the older FIFO fences and are
 * armed only around a wait for one.
 *
 * Left masked -- which is what "mask all sources until a waiter arms them"
 * did -- nothing ever announced a completion, so every fence wait fell back
 * to polling and a per-frame wait cost whatever the poll interval was.
 *
 * ERROR travels with it: a buffer the device refused has to be collected as
 * promptly as one it finished, or the slot stays busy. */
#define SVGA_IRQ_BASE_MASK (SVGA_IRQFLAG_COMMAND_BUFFER | SVGA_IRQFLAG_ERROR)

// ---------------------------------------------------------------------------
// Driver limits
// ---------------------------------------------------------------------------
#define SVGA_MAX_GMRS 64
#define SVGA_FENCE_TIMEOUT_US 2000000 // 2 s: fence wait give-up
#define SVGA_BUSY_TIMEOUT_US 2000000 // 2 s: legacy sync give-up

// ---------------------------------------------------------------------------
// Public driver API
// ---------------------------------------------------------------------------

// Lifecycle
int vmsvga2_init(void); // probe + bring-up; <0 => use GOP fallback
void vmsvga2_shutdown(void);
int vmsvga2_reset(void); // full reinit after FIFO corruption/VM reset
int vmsvga2_active(void); // 1 when driver owns the display

// Capabilities / limits
uint32_t vmsvga2_get_caps(void); // SVGA_CAP_* bits
uint32_t vmsvga2_get_vram_size(void);
uint32_t vmsvga2_get_max_width(void);
uint32_t vmsvga2_get_max_height(void);
// The resolution the host recommends: its own screen, asked for over the
// backdoor or read out of the display topology, once, at probe.  Returns <0
// when the hypervisor gave no answer.  Nothing to do with the boot
// framebuffer -- this is the display the virtual machine is shown on, not
// the mode list the virtual firmware happened to advertise.
int vmsvga2_get_host_preferred(uint32_t *width, uint32_t *height);

// Modesetting
int vmsvga2_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
int vmsvga2_display_enable(int enable);
int vmsvga2_get_info(framebuffer_info_t *out); // current scanout geometry
uint64_t vmsvga2_get_fb_phys(uint64_t *size_out); // FB BAR for mmap
void vmsvga2_set_traces(int enable); // host snoops direct VRAM writes

// Updates and synchronization
void vmsvga2_update_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void vmsvga2_update_full(void);
void vmsvga2_fifo_flush(void); // drain all queued commands
uint32_t vmsvga2_fence_insert(void); // returns fence value (0 if unavail)
uint32_t vmsvga2_fence_alloc(void);  // allocate only; caller submits the command
/* Tell this layer that something above it owns a command-buffer channel on
 * the same device: it then emits no FIFO fences of its own, so the device's
 * one fence register has a single writer.  See vmsvga2_fifo.c. */
void vmsvga2_set_cmdbuf_owner(int on);
/* ...and hand it the commands this layer still has to issue for that owner's
 * buffers (guest memory region define/remap/unbind), so they travel down the
 * same channel as the commands that name the regions, in order.  NULL puts
 * them back on the FIFO.  `ring' as in vmw_cmd_raw(): 0 batches, 1 announces. */
void vmsvga2_set_cmd_channel(int (*submit)(const void *cmds, uint32_t bytes,
					    int ring));
int vmsvga2_fence_passed(uint32_t fence);
int vmsvga2_fence_wait(uint32_t fence, uint64_t timeout_us);

// Cursor
int vmsvga2_cursor_define(uint32_t width, uint32_t height, uint32_t hot_x,
			  uint32_t hot_y, const uint32_t *and_mask,
			  const uint32_t *xor_mask);
int vmsvga2_cursor_define_alpha(uint32_t width, uint32_t height,
				uint32_t hot_x, uint32_t hot_y,
				const uint32_t *argb_pixels);
int vmsvga2_cursor_move(int32_t x, int32_t y, int visible);
int vmsvga2_cursor_show(int visible);
int vmsvga2_has_hw_cursor(void);

// GMR (guest memory regions)
int vmsvga2_gmr_alloc(uint32_t num_pages); // returns gmr id or <0
int vmsvga2_gmr_bind(int gmr_id, const uint64_t *page_phys,
		     uint32_t num_pages);
int vmsvga2_gmr_free(int gmr_id);

// IRQ entry (called from the central interrupt dispatcher).  Returns 1 when
// the device asserted the interrupt (status read+acked), 0 when the line was
// shared and this device was idle.
int vmsvga2_irq(void);
extern volatile int g_vmsvga_initialized;
extern volatile int g_vmsvga_legacy_irq;

// Boot-time best-fit mode selection (same preferred table as the bootloader)
void vmsvga2_setup_boot_mode(void);

// Mode setting and bring-up: facts read at probe, and the current
// mode's pixel layout.
// SVGA_REG_CAP2 (SVGA_CAP2_* bits), 0 without SVGA_CAP_CAP2_REGISTER or
// device.
uint32_t vmsvga2_get_caps2(void);
// The size to come up in when the host recommends nothing: the mode
// registers at probe raised to at least 1280x800, capped by the device
// maximum.  <0 without a device.
int vmsvga2_get_initial_size(uint32_t *width, uint32_t *height);
// SVGA_REG_RED/GREEN/BLUE_MASK of the console's current mode (RGB565 at
// 16 bpp, XRGB8888 at 32 -- other layouts are refused or logged by the mode
// set).  <0 while the console does not own the display.
int vmsvga2_get_rgb_masks(uint32_t *red, uint32_t *green, uint32_t *blue);

#endif // _KERNEL_DEV_VIDEO_VMSVGA2_H_
