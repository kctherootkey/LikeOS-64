// LikeOS -- VMware SVGA II hardware access exported to the
// display-manager driver (kernel/dev/gpu/vmwgfx).
//
// The boot console driver (kernel/dev/video/vmsvga2*.c) keeps the register
// file, the FIFO engine, the interrupt and its mask, and the hardware cursor;
// the display-manager driver builds its command streams, KMS and buffer
// objects on top of these.
//
// Includable beside the full device headers (svga/svga_reg.h, svga3d_*.h):
// vmsvga2.h takes its register definitions from them.
//
// Return convention of the interfaces added for the display-manager driver
// (the vmsvga2_irq_waiter_*, vmsvga2_hw_svga_*, vmsvga2_hw_cursor_* and
// vmsvga2_hw_fifo_try_reserve/_max_bytes group): 0 or a pointer on success,
// a negative errno or NULL on failure.  The older exports keep their 0 / -1
// convention.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_VIDEO_VMSVGA2_HW_H
#define KERNEL_DEV_VIDEO_VMSVGA2_HW_H

#include <kernel/uapi/types.h>
#include <kernel/hal/pci.h>
#include <kernel/dev/video/vmsvga2.h>

struct vmsvga2_hw_geometry {
	uint64_t fb_phys;
	uint8_t *fb_virt;
	uint32_t vram_size;
	uint32_t max_width, max_height;
	uint32_t width, height, bpp, pitch, fb_offset; /* current scanout */
	uint32_t caps, fifo_caps, fifo_size;
	int irq_enabled;
};

typedef void (*vmsvga2_hw_irq_cb_t)(uint32_t irq_status);

int vmsvga2_hw_present(void);
const pci_device_t *vmsvga2_hw_pci(void);
uint32_t vmsvga2_hw_read_reg(uint32_t index);
void vmsvga2_hw_write_reg(uint32_t index, uint32_t value);
int vmsvga2_hw_has_fifo_cap(uint32_t cap);
/* A FIFO register is present when the device declared it: one of the four
 * basic registers, or with SVGA_CAP_EXTENDED_FIFO one of the
 * SVGA_REG_MEM_REGS registers the device states it has.  The register area
 * this layer programs is rounded up to a page, but the padding beyond the
 * declared registers does not count; vmsvga2_hw_fifo_reg() reads 0 for
 * anything absent. */
int vmsvga2_hw_has_fifo_reg(uint32_t reg);
uint32_t vmsvga2_hw_fifo_reg(uint32_t reg);
void vmsvga2_hw_geometry(struct vmsvga2_hw_geometry *g);

/* ---- FIFO ---------------------------------------------------------------
 *
 * One atomic command from a buffer, or reserve/fill/commit.  Every entry
 * point refuses (returns -1 / NULL) a size that is not a multiple of 4 or is
 * larger than vmsvga2_hw_fifo_max_bytes(); split larger streams at command
 * boundaries. */
int vmsvga2_hw_fifo_submit(const void *data, uint32_t bytes);
/* ...queued without ringing; finish the run with vmsvga2_hw_doorbell(). */
int vmsvga2_hw_fifo_submit_batch(const void *data, uint32_t bytes);
/* Reserve `bytes' the caller fills in place, then vmsvga2_hw_fifo_commit()
 * with the same size, or vmsvga2_hw_fifo_abort().  The FIFO lock is taken
 * here (interrupts off) and released by the commit/abort: fill the buffer
 * without sleeping, without taking other driver locks, and without calling
 * back into any vmsvga2_* function.  When the ring is full it waits for the
 * host to drain space BEFORE taking the lock: a task with interrupts on
 * sleeps (up to 3 s), a caller with interrupts off spins (up to 100 ms).
 * The same holds for vmsvga2_hw_fifo_submit()/_submit_batch().  NULL:
 * device absent or wedged, or a size refused as above. */
void *vmsvga2_hw_fifo_reserve(uint32_t bytes);
/* The non-blocking form, for contexts that must not wait (atomic, console
 * flush, interrupt-adjacent): NULL -- with nothing held -- when the FIFO lock
 * is contended or the ring has no room for `bytes' right now, as well as in
 * every case vmsvga2_hw_fifo_reserve() returns NULL.  The caller keeps its
 * work and retries later.  On success exactly as vmsvga2_hw_fifo_reserve():
 * the lock is held until vmsvga2_hw_fifo_commit()/_abort(). */
void *vmsvga2_hw_fifo_try_reserve(uint32_t bytes);
void vmsvga2_hw_fifo_commit(uint32_t bytes);
void vmsvga2_hw_fifo_abort(void);
void vmsvga2_hw_doorbell(void);
/* The largest single submission/reservation the FIFO entry points accept
 * (the size of the bounce buffer that carries reservations wrapping the end
 * of the ring); 0 when the device is absent. */
uint32_t vmsvga2_hw_fifo_max_bytes(void);
/* FIFO engine: the FIFO register block carries a pitch lock
 * (SVGA_CAP_EXTENDED_FIFO and SVGA_FIFO_CAP_PITCHLOCK, read now, with a
 * declared register block that reaches SVGA_FIFO_PITCHLOCK); 0 when the
 * device is absent.  The same test the console's mode set uses. */
int vmsvga2_hw_fifo_have_pitchlock(void);

/* ---- Fences and the interrupt -------------------------------------------- */
/* The device's last-passed fence, the goal IRQ, the IRQ hook. */
int vmsvga2_hw_has_fence(void);
uint32_t vmsvga2_hw_fence_current(void);
/* Ask for an interrupt when the device passes fence `goal': write
 * SVGA_FIFO_FENCE_GOAL (never moving an outstanding goal backwards) and hold
 * the fence interrupt sources (ANY_FENCE and the goal source, see
 * vmsvga2_irq_fence_goal_flag()) armed while the goal is outstanding.  The
 * hold is one refcounted waiter, whatever the number of calls, and is let go
 * as soon as the device has passed the goal -- by the interrupt that says
 * so, or by this call when the fence had already gone by.  Nothing happens
 * without a routed interrupt or a FIFO with the goal register.  Any
 * context.  (With VMSVGA2_IRQ_GOAL_HOLD set to 0 in vmsvga2_irq.c the
 * sources are instead armed until the next device reset, uncounted.) */
void vmsvga2_hw_set_fence_goal(uint32_t goal);
/* Called from the interrupt handler, interrupts off, with the status bits
 * the device raised (already acknowledged).  Must not sleep. */
void vmsvga2_hw_set_irq_callback(vmsvga2_hw_irq_cb_t cb);

/* Refcounted interrupt sources.
 *
 * `flag' is one or more SVGA_IRQFLAG_* bits (below SVGA_IRQFLAG_MAX).  Every
 * bit has its own waiter count; SVGA_REG_IRQMASK always holds
 *     SVGA_IRQ_BASE_MASK | (bits with a count > 0)
 * -- the fence wait of this layer and the goal hold of
 * vmsvga2_hw_set_fence_goal() are counted waiters too (only with
 * VMSVGA2_IRQ_GOAL_HOLD off does the goal arm its bits until the next reset)
 * -- and is written only by this layer.  The first add of a bit acknowledges
 * any status already latched for it and then unmasks it, so the caller
 * re-checks its condition after the add and only then waits; the last
 * remove masks it again.  Each add must be paired with one remove of the
 * same bits.  Callable from any context, including with interrupts off (the
 * counts are under a spinlock; no sleeping).  On a device without
 * SVGA_CAP_IRQMASK, or with the interrupt not routed, the counts are kept
 * but nothing is written: waiters must poll (vmsvga2_hw_geometry().
 * irq_enabled says which).  The counts survive vmsvga2_reset() and a
 * suspend (vmsvga2_hw_suspend()/_resume()), which re-program the mask from
 * them. */
void vmsvga2_irq_waiter_add(uint32_t flag);
void vmsvga2_irq_waiter_remove(uint32_t flag);

/* Interrupt and fence wakeups.
 *
 * The same counts, returning true when the mask register was reprogrammed
 * (first add / last remove of any of the bits) -- after a true add the
 * condition may have changed while the source was masked, so look again:
 *   generic: any SVGA_IRQFLAG_* bits;
 *   seqno:   SVGA_IRQFLAG_ANY_FENCE (the fence register moved);
 *   goal:    vmsvga2_irq_fence_goal_flag() (the fence register reached
 *            SVGA_FIFO_FENCE_GOAL): SVGA_IRQFLAG_REG_FENCE_GOAL on a device
 *            with SVGA_CAP2_EXTRA_REGS, SVGA_IRQFLAG_FENCE_GOAL otherwise.
 *
 * vmsvga2_hw_set_fence_goal() (with VMSVGA2_IRQ_GOAL_HOLD, the default)
 * holds one ANY_FENCE + goal waiter while its goal is outstanding and lets
 * go when the device passes it, instead of arming until the next reset; the
 * goal only ever moves forward while one is outstanding.
 *
 * vmsvga2_irq_error_count(): SVGA_IRQFLAG_ERROR interrupts seen so far. */
bool vmsvga2_irq_generic_waiter_add(uint32_t flag);
bool vmsvga2_irq_generic_waiter_remove(uint32_t flag);
bool vmsvga2_irq_seqno_waiter_add(void);
bool vmsvga2_irq_seqno_waiter_remove(void);
bool vmsvga2_irq_goal_waiter_add(void);
bool vmsvga2_irq_goal_waiter_remove(void);
uint32_t vmsvga2_irq_fence_goal_flag(void);
uint32_t vmsvga2_irq_error_count(void);

/* ---- Display hand-over and mode setting ---------------------------------- */
/* Display hand-over between the console and a display-manager master. */
void vmsvga2_hw_display_take(void);
void vmsvga2_hw_display_release(void);
int vmsvga2_hw_display_taken(void);
int vmsvga2_hw_set_mode(uint32_t width, uint32_t height);
/* ...and the same without the console following it, for a console that
 * scans out of a buffer object rather than out of this device's memory. */
int vmsvga2_hw_set_mode_device(uint32_t width, uint32_t height);
/* Neither resets the device (SVGA_REG_ENABLE through 0) before the mode
 * registers unless VMSVGA2_HW_SET_MODE_RESETS is set to 1 in vmsvga2_hw.c; a
 * caller that wants the reset asks for it explicitly with
 * vmsvga2_hw_svga_disable(false) before the mode set.  The mode set itself
 * locks the pitch, writes the pixel size only with SVGA_CAP_8BIT_EMULATION,
 * refuses a colour-depth mismatch (registers put back) and writes the
 * one-display topology with SVGA_CAP_DISPLAY_TOPOLOGY. */

/* SVGA_REG_ENABLE without the mode-set sequence.
 *
 * vmsvga2_hw_svga_enable() writes SVGA_REG_ENABLE_ENABLE: the device scans
 * out of its legacy framebuffer (VRAM at the current mode).
 * vmsvga2_hw_svga_disable(hide) with hide=true writes
 * SVGA_REG_ENABLE_ENABLE | SVGA_REG_ENABLE_HIDE: the device stays enabled
 * -- FIFO, command buffers, guest-backed objects and screen targets keep
 * working -- but the legacy framebuffer is no longer shown, which is what a
 * driver presenting through screen objects/targets wants.  hide=false
 * writes SVGA_REG_ENABLE_DISABLE: legacy SVGA off, the device falls back to
 * its VGA personality.
 *
 * Both take the mode-set mutex, so they may sleep: process context only.
 * No-ops when the device is absent.  The console's mode sets and its
 * blanking (vmsvga2_set_mode, vmsvga2_display_enable) use the same register
 * sequences.  vmsvga2_hw_svga_enable() writes the register only when
 * the device is not already enabled and unhidden. */
void vmsvga2_hw_svga_enable(void);
void vmsvga2_hw_svga_disable(bool hide);

/* ---- Hardware cursor -------------------------------------------------------
 *
 * The device's own cursor (SVGA_CMD_DEFINE_CURSOR / _DEFINE_ALPHA_CURSOR
 * images, position through the FIFO cursor-bypass registers or the legacy
 * cursor registers).  Images are at most 64x64.  The last image defined is
 * re-sent after a device reset.  Return 0, -ENODEV when the device (or the
 * capability the call needs) is absent, -EINVAL for a bad argument, -EIO
 * when the define command could not be queued.  move/show/hide never wait.
 * A define may wait to hand its command over -- for FIFO space or a command
 * buffer, sleeping in a context with interrupts on, spinning (bounded) where
 * interrupts are off -- but never with a driver lock held.
 *
 *   define_mono:  1 bpp AND and XOR masks, each row padded to 32 bits:
 *                 ((width + 31) / 32) * height dwords per mask.
 *                 Needs SVGA_CAP_CURSOR.
 *   define_alpha: width * height 32-bit ARGB pixels (alpha in bits
 *                 31..24), rows packed, passed to the device unchanged.
 *                 Needs SVGA_CAP_ALPHA_CURSOR.
 *   move:         position of the hotspot in screen coordinates, and
 *                 whether the cursor is shown there.  Needs the console to
 *                 own the display mode (vmsvga2_active()) and a position
 *                 mechanism (vmsvga2_has_hw_cursor()).
 *   show / hide:  visibility only; hide is asserted through both the FIFO
 *                 bypass and the legacy register (see vmsvga2_cursor.c).
 */
int vmsvga2_hw_cursor_define_mono(uint32_t width, uint32_t height,
				  uint32_t hot_x, uint32_t hot_y,
				  const uint32_t *and_mask,
				  const uint32_t *xor_mask);
int vmsvga2_hw_cursor_define_alpha(uint32_t width, uint32_t height,
				   uint32_t hot_x, uint32_t hot_y,
				   const uint32_t *argb_pixels);
int vmsvga2_hw_cursor_move(int32_t x, int32_t y, bool show);
int vmsvga2_hw_cursor_show(void);
int vmsvga2_hw_cursor_hide(void);

/* ---- cursor position paths, image ownership, GMR limits -------------
 *
 * Position mechanism, chosen per call from the device's capabilities (best
 * first; vmsvga2_hw_cursor_path() says which one a call would take):
 *   VMSVGA2_CURSOR_PATH_CURSOR4  SVGA_REG_CURSOR4_X/_Y/_SCREEN_ID/_ON, then
 *                                _SUBMIT (SVGA_CAP2_EXTRA_REGS);
 *   VMSVGA2_CURSOR_PATH_BYPASS3  SVGA_FIFO_CURSOR_ON/_X/_Y, then
 *                                SVGA_FIFO_CURSOR_COUNT bumped
 *                                (SVGA_FIFO_CAP_CURSOR_BYPASS_3 and a FIFO
 *                                register block reaching CURSOR_COUNT);
 *   VMSVGA2_CURSOR_PATH_LEGACY   SVGA_REG_CURSOR_X/_Y/_ON
 *                                (SVGA_CAP_CURSOR_BYPASS or _BYPASS_2);
 *   VMSVGA2_CURSOR_PATH_NONE     no position mechanism (or no device).
 * A hide (move/update_position with show false, hide()) through the FIFO
 * bypass is also written to SVGA_REG_CURSOR_ON: some hosts honour only that
 * one.  The CURSOR4 path writes the CURSOR4 block alone.
 *
 * Defines (define_mono/_alpha and vmsvga2_cursor_define*): once a
 * command-buffer owner has set a channel (vmsvga2_set_cmd_channel()), a
 * define from a context that may sleep (interrupts on, a current task) goes
 * down that channel, flushed (ring = 1), with no driver lock held; from any
 * other context it goes into the FIFO.  A define issued while another is on
 * its way to the channel only updates the cached image, which that sender
 * then sends before returning: the host ends up with the newest image.
 *
 *   update_position: position and visibility, written on every call, for a
 *                 driver presenting through screen objects/targets: needs
 *                 only the device and a position path (not
 *                 vmsvga2_active()).  Screen id is always "none" (positions
 *                 are in the virtual-desktop coordinates).  Spinlock only;
 *                 callable from any context.  0, or -ENODEV.
 *   forget:       drop the cached image without touching the host, for a
 *                 driver whose image now comes from elsewhere (a cursor MOB
 *                 through SVGA_REG_CURSOR_MOBID): a device reset then does
 *                 not define the stale cached image over it.  The next
 *                 define_* caches and sends a new image as usual.
 *   gmr_limits:   SVGA_REG_GMR_MAX_IDS (clamped to SVGA_MAX_GMRS, the ids
 *                 vmsvga2_gmr_alloc() hands out) and SVGA_REG_GMRS_MAX_PAGES
 *                 (0 without SVGA_CAP_GMR2), as read once at probe; both 0
 *                 when the device or GMR support is absent.  Either pointer
 *                 may be NULL.
 *
 * vmsvga2_gmr_bind() on GMR2 sends DEFINE_GMR2 and then REMAP_GMR2 commands
 * of at most 31 KB of page numbers each (32-bit page numbers when every page
 * lies below 16 TB, else 64-bit), down the command channel when one is set,
 * else into the FIFO -- in one reservation when the whole sequence fits
 * vmsvga2_hw_fifo_max_bytes(), else one reservation per command.  Process
 * context (it may wait for FIFO space or a command buffer).
 * vmsvga2_gmr_free() unbinds (DEFINE_GMR2 of zero pages) before the id can
 * be allocated again. */
enum vmsvga2_cursor_path {
	VMSVGA2_CURSOR_PATH_NONE = 0,
	VMSVGA2_CURSOR_PATH_CURSOR4 = 1,
	VMSVGA2_CURSOR_PATH_BYPASS3 = 2,
	VMSVGA2_CURSOR_PATH_LEGACY = 3,
};
int vmsvga2_hw_cursor_path(void);
int vmsvga2_hw_cursor_update_position(bool show, int32_t x, int32_t y);
void vmsvga2_hw_cursor_forget(void);
void vmsvga2_hw_gmr_limits(uint32_t *max_ids, uint32_t *max_pages);

/* ---- Power management ------------------------------------------------------
 *
 * Taking the device down and bringing it back, for the display-manager
 * driver's suspend/resume (and, through the same quiesce, vmsvga2_shutdown()).
 * 0 keeps the hardware layer without it: both calls return -EOPNOTSUPP and
 * vmsvga2_shutdown() does what it did before.
 */
#ifndef VMSVGA2_PM
#define VMSVGA2_PM 1
#endif

/* vmsvga2_hw_suspend(reset)
 *
 * Quiesce: everything queued in the FIFO is drained (a fence, or a full
 * SVGA_REG_SYNC/SVGA_REG_BUSY drain where no fence can be used), then a
 * legacy sync on top; the interrupt is masked and acknowledged and no longer
 * claimed (the waiter counts are kept); the scan-out state, SVGA_REG_ENABLE
 * and SVGA_REG_TRACES are remembered.  From here until vmsvga2_hw_resume()
 * this layer is "suspended": every FIFO reservation is refused at once
 * (submit/reserve return -1/NULL, fence inserts return 0, drains return at
 * once), the console's screen updates are dropped (the console is redrawn
 * whole on resume), and mode sets, blanking and SVGA_REG_ENABLE changes are
 * refused or ignored.  The cursor position calls keep working (registers
 * only) and the last position is put back on resume.
 *
 * reset=false (light): the device is left running as it is -- enabled,
 * SVGA_REG_CONFIG_DONE set, every object, region and command-buffer context
 * on it intact.
 *
 * reset=true (full): the device is then handed back the way the driver
 * found it and reset: SVGA_REG_CONFIG_DONE and SVGA_REG_TRACES get their
 * values from probe, SVGA_REG_ENABLE is written 0 (which resets the device:
 * the FIFO stops, guest memory regions, guest-backed objects, screen
 * objects/targets and command-buffer contexts are gone), and bus mastering
 * is turned off in the PCI command register.  Refused with -EBUSY while a
 * command channel is registered (vmsvga2_set_cmd_channel()): the owner of the
 * command buffers takes its manager down first -- its contexts would be lost
 * under it -- and with it the channel, so that what this layer replays on
 * resume goes into the FIFO, the one stream that exists again then.
 *
 * Process context with interrupts on (it waits for the drain and takes the
 * mode-set mutex).  The caller has stopped its own submissions and waited
 * for its own fences/command buffers first: nothing may be added to the FIFO
 * or a command buffer while the device is suspended.
 * 0, -ENODEV (no device), -EBUSY (already suspended, or reset with a channel
 * registered), -EINVAL (atomic context), -EOPNOTSUPP (VMSVGA2_PM 0). */
int vmsvga2_hw_suspend(bool reset);

/* vmsvga2_hw_resume(fence_seed)
 *
 * The PCI command register gets I/O, memory decoding and bus mastering
 * again; the device version is negotiated again and the capabilities read
 * again (a change is logged; the probe-time capability set stays in force,
 * the FIFO capabilities of the re-created FIFO are taken).
 *
 * Then, after a full suspend -- or after a light one whose device turns out
 * to have lost its state (SVGA_REG_CONFIG_DONE or SVGA_REG_ENABLE reading 0,
 * or a FIFO whose pointers no longer make sense) -- the device is set up the
 * way probe set it up: SVGA_REG_ENABLE as it was at suspend (enabled and
 * hidden when it was off then: the FIFO only runs on an enabled device),
 * SVGA_REG_TRACES as at suspend, the FIFO laid out empty again (MIN, MAX,
 * NEXT_CMD, STOP, BUSY, RESERVED) with SVGA_REG_CONFIG_DONE 1, SVGA_REG_GUEST_ID,
 * and the FIFO fence register seeded with the last fence value handed out --
 * the later of `fence_seed' and this layer's own counter (vmsvga2_fence_alloc()
 * / _insert() hand out every value, so 0 is a fine seed: "this layer's
 * counter") -- so every fence issued before the suspend reads as passed and
 * the next one continues the sequence.  After a light suspend whose device
 * kept its state none of that is touched (the fence register keeps what the
 * device wrote; the counter only moves up to `fence_seed').
 *
 * Then, either way: the interrupt is claimed again and its mask programmed
 * from the waiter counts that lived through the suspend; the suspended state
 * ends; the last cursor position and visibility are written again (after a
 * re-initialization the cached cursor image is defined again first, through
 * the FIFO); after a re-initialization the legacy
 * (descriptor-programmed, no SVGA_CAP_GMR2) guest memory regions are
 * programmed again from the descriptor pages this layer keeps for them; and
 * when the console owns the display (it scans out of video memory through
 * this layer: no display-manager master holds it and the console's screen
 * updates still come here) its mode is set again, blanked again if it was
 * blanked, and redrawn.
 *
 * Replayed by this layer: the FIFO, its fence register, the interrupt mask,
 * the cursor image and position, legacy descriptor regions, the console's
 * mode.  NOT replayed -- the display-manager driver's, after a return of 1:
 * GMR2 regions (rebound through vmsvga2_gmr_bind() with the same id and page
 * list; the id stays allocated across the suspend), guest-backed object
 * tables and MOBs, command-buffer contexts and their channel registration,
 * screen objects/targets and the display-manager's modes (the DRM resume
 * replay), the cursor MOB.  Rebinding a legacy descriptor region as well is
 * harmless (the old descriptor page is replaced and freed).
 *
 * Process context.  Returns 1 when the device was re-initialized (its
 * objects are gone: the caller rebuilds its state), 0 when the device kept
 * its state (light suspend) or was not suspended, <0 on failure: -ENODEV,
 * -EIO (the device does not answer version negotiation any more, its FIFO
 * moved, or the FIFO it was given is unusable -- it stays suspended),
 * -EINVAL (atomic context), -EOPNOTSUPP (VMSVGA2_PM 0). */
int vmsvga2_hw_resume(uint32_t fence_seed);

/* 1 between vmsvga2_hw_suspend() and the vmsvga2_hw_resume() that ends it
 * (and after vmsvga2_shutdown()), else 0. */
int vmsvga2_hw_suspended(void);

#endif
