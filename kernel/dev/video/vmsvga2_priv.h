// LikeOS VMware SVGA II display driver -- state and interfaces shared by the
// driver's source files (vmsvga2.c, vmsvga2_hw.c, vmsvga2_fifo.c,
// vmsvga2_irq.c, vmsvga2_gmr.c, vmsvga2_cursor.c).  Not for use outside
// kernel/dev/video/: other drivers go through vmsvga2.h / vmsvga2_hw.h.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_VIDEO_VMSVGA2_PRIV_H
#define KERNEL_DEV_VIDEO_VMSVGA2_PRIV_H

#include <kernel/dev/video/vmsvga2.h>
#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/uapi/bug.h>
#include <kernel/io/console.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/interrupt.h> // inl/outl
#include <kernel/ke/waitq.h> // interrupt wait channels

// Register 24 was the cursor id register of the first cursor interface; the
// device headers name it SVGA_REG_DEAD now.  The legacy cursor path still
// writes 0 to it before the position registers, as it always has.
#define VMSVGA2_REG_CURSOR_ID SVGA_REG_DEAD

// "No screen": what the cursor-bypass screen id register takes to mean the
// whole legacy display.
#ifndef SVGA_ID_INVALID_SCREEN
#define SVGA_ID_INVALID_SCREEN 0xFFFFFFFFU
#endif

// Bounds of the FIFO bounce buffer (see svga_fifo_bounce_size()).
#define SVGA_FIFO_BOUNCE_MIN (16U * 1024U)
#define SVGA_FIFO_BOUNCE_MAX (1024U * 1024U)

// ===========================================================================
// Driver state
// ===========================================================================

typedef struct {
	// Device identity / resources
	const pci_device_t *pci;
	uint16_t io_base; // BAR0 I/O port base
	uint64_t fb_phys; // BAR1: framebuffer (VRAM)
	uint64_t fifo_phys; // BAR2 / SVGA_REG_MEM_START: command FIFO
	uint32_t fifo_size; // bytes
	volatile uint32_t *fifo; // mapped FIFO (UC)
	uint8_t *fb_virt; // mapped framebuffer (direct map, WC)

	uint32_t version; // negotiated SVGA_VERSION_*
	uint32_t caps; // SVGA_CAP_*
	uint32_t fifo_caps; // SVGA_FIFO_CAP_* (0 unless extended FIFO)
	uint32_t vram_size;
	uint32_t fb_size;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t host_bpp;

	// The resolution the host recommends, sampled once at probe; 0 when
	// the hypervisor did not answer (see svga_query_host_preferred()).
	uint32_t pref_width;
	uint32_t pref_height;
	const char *pref_source; // where the answer came from, for the log
	// What the topology registers said, kept even when it was discounted,
	// so the log can say why there was no recommendation.
	uint32_t topo_width;
	uint32_t topo_height;

	// CRTC state (current scanout mode)
	struct {
		uint32_t width;
		uint32_t height;
		uint32_t bpp;
		uint32_t pitch;
		uint32_t fb_offset;
		int enabled;
	} crtc;

	// FIFO bounce buffer for reservations that wrap the ring.  Sized at
	// probe and never reallocated: every reservation is made under
	// svga_fifo_lock with interrupts off, where nothing may allocate.
	// bounce_size is also the largest command the FIFO accepts.
	uint8_t *bounce;
	uint32_t bounce_size;
	uint32_t reserved_bytes; // active reservation size
	int reserved_in_place; // reservation points into the ring

	// Deferred error accounting.  The FIFO submit path runs inside the
	// console flush hook — i.e. potentially under console_lock — where
	// printing (kprintf/WARN) would self-deadlock.  Failures are counted
	// here and reported by svga_report_errors() from safe contexts.
	volatile uint32_t err_fifo_corrupt;
	volatile uint32_t err_fifo_stall;
	volatile uint32_t err_fifo_oversize;
	uint32_t err_reported_corrupt;
	uint32_t err_reported_stall;
	uint32_t err_reported_oversize;

	// Fences
	uint32_t next_fence;

	// IRQ
	int irq_enabled; // SVGA_CAP_IRQMASK negotiated + routed
	uint32_t irq_pending; // accumulated SVGA_IRQFLAG_* (under irq_lock)

	int active; // display owned by this driver
	int present; // device probed successfully

	// FIFO engine, vmsvga2_fifo.c.
	uint32_t fifo_min; // SVGA_FIFO_MIN as programmed (register area)
	// FIFO registers the device declared (4 without the extended FIFO);
	// what svga_has_fifo_reg() answers by.
	uint32_t fifo_num_regs;
	int fifo_busy_reg; // SVGA_FIFO_BUSY idle handshake usable
	volatile uint32_t err_fifo_dead; // waits ended by SVGA_FIFO_DEAD
	volatile uint32_t err_fifo_misuse; // commit beyond its reservation
	uint32_t err_reported_dead;
	uint32_t err_reported_misuse;

	// Between vmsvga2_hw_suspend() and vmsvga2_hw_resume(), and after
	// vmsvga2_shutdown(): the FIFO refuses every reservation, drains return
	// at once, the console's updates are dropped and mode sets refused
	// (vmsvga2.c).  Set and cleared under the mode-set mutex; read
	// lock-free (a producer racing the suspend is drained by it).
	volatile int pm_suspended;
} svga_state_t;

extern svga_state_t g_svga;

// Locks (see vmsvga2.h for the locking model)
extern spinlock_t svga_reg_lock; // vmsvga2_hw.c
extern spinlock_t svga_fifo_lock; // vmsvga2_fifo.c
extern spinlock_t svga_irq_lock; // vmsvga2_irq.c

// ===========================================================================
// Register access (vmsvga2_hw.c)
// ===========================================================================

uint32_t svga_read_reg(uint32_t index);
void svga_write_reg(uint32_t index, uint32_t value);

// Ask the host to start processing the FIFO (asynchronous doorbell)
static inline void svga_doorbell(void)
{
	svga_write_reg(SVGA_REG_SYNC, 1);
}

// Is FIFO register `reg' one the device has?  Answered by what the device
// declared -- four registers without SVGA_CAP_EXTENDED_FIFO, SVGA_REG_MEM_REGS
// of them with it (see svga_fifo_init()) -- and never beyond the register
// area programmed into SVGA_FIFO_MIN.  Not by SVGA_FIFO_MIN alone: that is
// rounded up to a page, and the padding between the last register the device
// declared and the start of the ring is memory nobody writes, which a caller
// would otherwise take for a register reading 0 -- or, worse, for one
// holding whatever was left there.
static inline int svga_has_fifo_reg(uint32_t reg)
{
	return g_svga.fifo && reg < g_svga.fifo_num_regs &&
	       g_svga.fifo[SVGA_FIFO_MIN] > reg * sizeof(uint32_t);
}

static inline int svga_has_fifo_cap(uint32_t cap)
{
	return (g_svga.fifo_caps & cap) != 0;
}

// ===========================================================================
// FIFO engine and fences (vmsvga2_fifo.c)
// ===========================================================================

uint32_t svga_fifo_bounce_size(uint32_t fifo_size);
int svga_fifo_init(void);
void svga_fifo_reset_ring(void);
void svga_report_errors(void);
void *svga_fifo_reserve(uint32_t bytes);
void *svga_fifo_reserve_nowait(uint32_t bytes);
void svga_fifo_commit(uint32_t bytes);
int svga_fifo_write_cmd_quiet(const void *cmd, uint32_t bytes);
int svga_fifo_write_cmd(const void *cmd, uint32_t bytes);
int svga_has_fence(void);
int svga_legacy_sync(void);
// Refuse every further reservation (sets g_svga.pm_suspended), let a
// producer already holding the lock finish, then SYNC/BUSY-drain the device.
// 0, or <0 when the drain timed out.  Process context.  The flag is cleared
// by the resume (vmsvga2.c) once the FIFO is usable again.
int svga_fifo_quiesce(void);

// The command channel of a command-buffer owner, or NULL (see
// vmsvga2_set_cmd_channel()).
extern int (*g_cmd_channel)(const void *cmds, uint32_t bytes, int ring);

// FIFO engine, vmsvga2_fifo.c.
//
// Why the host is told (the value written to SVGA_REG_SYNC).
#ifndef SVGA_SYNC_GENERIC
#define SVGA_SYNC_GENERIC 1
#endif
#ifndef SVGA_SYNC_FIFOFULL
#define SVGA_SYNC_FIFOFULL 2
#endif
// Announce new work: SVGA_REG_SYNC only when the host is idle (the
// SVGA_FIFO_BUSY handshake), every time on a device without the register.
// svga_doorbell() above rings unconditionally.
void svga_fifo_ping_host(uint32_t reason);
// Take svga_fifo_lock (interrupt state saved in *flags) and reserve: the
// lock is held on success until svga_fifo_commit_irqrestore() /
// svga_fifo_abort_irqrestore(); NULL means nothing is held.  When the ring
// is full the lock is dropped while waiting: a task with interrupts on
// sleeps (FIFO-progress interrupt, or yields where there is none) for up to
// 3 s; with interrupts off it spins for at most 100 ms.  The _try_ form
// never waits (NULL when contended or full).  svga_fifo_reserve() and
// svga_fifo_reserve_nowait() above are the same with the lock already held
// by the caller; svga_fifo_reserve() drops and retakes it to wait.
void *svga_fifo_reserve_irqsave(uint32_t bytes, uint64_t *flags);
void *svga_fifo_try_reserve_irqsave(uint32_t bytes, uint64_t *flags);
// Commit (svga_fifo_commit()), drop the lock, and with `ping' announce.
void svga_fifo_commit_irqrestore(uint32_t bytes, uint64_t flags, int ping);
void svga_fifo_abort_irqrestore(uint64_t flags);
// One command, announced, never waiting -- for the console flush hook:
// 0, -EAGAIN (lock contended or ring full right now: keep the update and
// retry later) or -EIO (refused, device absent).
int svga_fifo_try_write_cmd(const void *cmd, uint32_t bytes);
// The FIFO register block carries a pitch lock (SVGA_CAP_EXTENDED_FIFO,
// SVGA_FIFO_CAP_PITCHLOCK read from the FIFO now, and a register block that
// reaches SVGA_FIFO_PITCHLOCK).  The one test for it: the mode set's pitch
// lock (vmsvga2_hw.c) and the export to the display-manager driver use it.
int svga_fifo_have_pitchlock(void);

// ===========================================================================
// Interrupt (vmsvga2_irq.c)
// ===========================================================================

void svga_irq_init(void);
void svga_irq_rearm(void);
// After svga_irq_uninstall(): claim the line routed at probe again,
// acknowledge what is latched, program the mask from the waiter counts that
// lived through the gap, let a fence-goal hold go whose goal has passed, and
// send every waiter back to look.  Nothing without a routed line.  Process
// context (prints).
void svga_irq_reinstall(void);
void svga_irq_arm_until_reset(uint32_t flags);

// Interrupt and fence wakeups, vmsvga2_irq.c.
//
// Wait channels the interrupt wakes: g_svga_fence_wq when a fence passes
// (SVGA_IRQFLAG_ANY_FENCE, the goal), g_svga_fifo_wq when the host consumed
// FIFO commands (SVGA_IRQFLAG_FIFO_PROGRESS).
extern struct wait_queue_head g_svga_fence_wq;
extern struct wait_queue_head g_svga_fifo_wq;

// Acknowledge the latched status (init, reset); mask everything, acknowledge
// and stop claiming the line (shutdown).
void svga_irq_preinstall(void);
void svga_irq_uninstall(void);

// The goal source this device raises (FENCE_GOAL, or REG_FENCE_GOAL with
// SVGA_CAP2_EXTRA_REGS), and the goal write: SVGA_FIFO_FENCE_GOAL := seq
// unless an outstanding goal already lies beyond it.  Any context.
uint32_t svga_irq_fence_goal_flag(void);
void svga_irq_fence_goal_set(uint32_t seq);

// FIFO-progress waiters (refcounted SVGA_IRQFLAG_FIFO_PROGRESS); true when
// the mask register was reprogrammed (re-check the condition).
bool svga_fifo_waiter_add(void);
bool svga_fifo_waiter_remove(void);

// Device error interrupts, counted by the handler; prints a rate-limited
// summary when the count moved.  Process context only (prints).
void svga_irq_report_errors(void);

// Wait until cond(arg) returns nonzero.
//
// With the interrupt available (routed, SVGA_CAP_IRQMASK) and a sleeping
// context, `irq_flags' are held as refcounted waiters for the duration and
// the caller sleeps on `wq' (g_svga_fence_wq / g_svga_fifo_wq), lost-wakeup
// safe, with a two-tick safety net.  Otherwise it polls: yields between
// looks in process context, spins with interrupts off or before the
// scheduler runs.  Returns 0, -ETIMEDOUT after `timeout_us', or -EINTR
// (SVGA_WAIT_INTERRUPTIBLE) on a pending signal.
//
// `cond' is called with interrupts off at times: it must not sleep or take
// a lock that is taken with interrupts on.  The caller must not hold a
// spinlock (it may sleep).  Ringing the doorbell / writing the goal is the
// caller's job, before the call.
#define SVGA_WAIT_INTERRUPTIBLE 0x1U
#define SVGA_WAIT_NUDGE 0x2U // polling: read SVGA_REG_BUSY each pass
int svga_irq_wait_event(struct wait_queue_head *wq, uint32_t irq_flags,
			int (*cond)(void *arg), void *arg,
			uint64_t timeout_us, unsigned int flags);

// ===========================================================================
// Hardware cursor (vmsvga2_cursor.c)
// ===========================================================================

void svga_cursor_restore(void);
// After a suspend: with `redefine' (the device lost its state) the cached
// image defined again (svga_cursor_restore()), then either way the last
// position and visibility written through the position path.
void svga_cursor_resume(int redefine);

// Cursor and guest memory regions: probe-time logging, called by
// vmsvga2_init() once the capabilities are known and the FIFO is up.  The
// facts both go by (SVGA_REG_CAP2 for the CURSOR4 registers, the region id
// and page limits) are the ones read once at probe, svga_device_caps2() and
// svga_device_gmr_limits().
//   svga_cursor_init()  logs which position path is in use;
//   svga_gmr_init()     logs the region limits (vmsvga2_gmr.c).
void svga_cursor_init(void);
void svga_gmr_init(void);

// After the device was re-initialized: program every bound legacy
// (descriptor) region again from the descriptor page kept for it.  Returns
// how many were; *gmr2_bound (may be NULL) gets the number of GMR2 regions
// still allocated and bound -- those are their owner's to bind again
// (vmsvga2_gmr_bind()).  Process context, device up, FIFO usable.
uint32_t svga_gmr_replay(uint32_t *gmr2_bound);

// ===========================================================================
// Mode setting, topology, bring-up: vmsvga2.c, vmsvga2_hw.c
// ===========================================================================

// Device facts read once at probe (vmsvga2_init()), and the pixel layout of
// the current mode.  Written only by vmsvga2.c.
typedef struct {
	// Set once the fields below up to max_primary_mem hold what the
	// device said; read through svga_device_caps2() /
	// svga_device_gmr_limits(), which read them on first use when probe
	// has not got that far yet.
	volatile int limits_read;
	uint32_t caps2; // SVGA_REG_CAP2; 0 without SVGA_CAP_CAP2_REGISTER
	// SVGA_REG_GMR_MAX_IDS as the device states it (SVGA_CAP_GMR or
	// SVGA_CAP_GMR2), else 0
	uint32_t gmr_max_ids;
	uint32_t gmr_max_pages; // SVGA_REG_GMRS_MAX_PAGES (SVGA_CAP_GMR2), else 0
	// Guest memory the host lets surfaces use beyond VRAM:
	// SVGA_REG_MEMORY_SIZE - VRAM with SVGA_CAP_GMR2, else 512 MiB.
	uint32_t memory_size;
	// Largest primary surface: SVGA_REG_MAX_PRIMARY_MEM with
	// SVGA_CAP_GBOBJECTS, else the VRAM size.
	uint32_t max_primary_mem;
	// The size to come up in when the host recommends nothing: the mode
	// registers at probe, raised to at least 1280x800 and capped by the
	// device maximum (1280x800 when they exceed it).
	uint32_t initial_width;
	uint32_t initial_height;
	// SVGA_REG_ENABLE / _CONFIG_DONE / _TRACES as found at probe, put
	// back by vmsvga2_shutdown() when state_saved.
	uint32_t saved_enable;
	uint32_t saved_config_done;
	uint32_t saved_traces;
	int state_saved;
	// The current mode's SVGA_REG_DEPTH and channel masks.
	uint32_t depth;
	uint32_t red_mask;
	uint32_t green_mask;
	uint32_t blue_mask;
	int masks_reported; // an unexpected 32-bpp layout was logged
} svga_devinfo_t;

extern svga_devinfo_t g_svga_info;

// The device facts above, from any context: read at probe, or on first use
// before that (the registers are constant, so two racing first readers get
// the same answer).  0 / no limits while the device's capabilities are not
// known yet.  max_pages may be NULL.
uint32_t svga_device_caps2(void);
void svga_device_gmr_limits(uint32_t *max_ids, uint32_t *max_pages);

// The console's screen updates (vmsvga2.c).  vmsvga2_update_rect() never
// waits for the FIFO: an update the FIFO cannot take right now (lock held
// elsewhere, ring full) is merged into a pending box, which goes out ahead of
// the next update -- and from the FIFO's own release points: every
// svga_fifo_commit_irqrestore()/svga_fifo_abort_irqrestore() calls this after
// dropping the lock, as does vmsvga2_fifo_flush() once the ring has drained.
// Never waits, any context; 0 when nothing is left pending, -EAGAIN when the
// box is still pending (a producer holding the FIFO lock right now sends it
// when it lets go).
int svga_console_flush_pending(void);

// Register sequences of a legacy mode set (vmsvga2_hw.c).
uint32_t svga_read_pitchlock(void);
void svga_write_pitchlock(uint32_t pitch);
uint32_t svga_format_depth(uint32_t bpp);
int svga_kms_write_svga(uint32_t width, uint32_t height, uint32_t pitch,
			uint32_t bpp, uint32_t depth);
void svga_ldu_commit_topology(uint32_t width, uint32_t height);
void svga_scanout_enable(void);
void svga_scanout_disable(int hide);

// Mode set (vmsvga2.c); `reset' cycles SVGA_REG_ENABLE through 0 first,
// which resets the device.  vmsvga2_set_mode() is the form without.
int svga_set_mode(uint32_t width, uint32_t height, uint32_t bpp, int reset);

#endif
