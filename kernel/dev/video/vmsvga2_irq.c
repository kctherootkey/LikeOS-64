// LikeOS VMware SVGA II display driver -- the device interrupt.
//
// Routing of the INTx line (ACPI _PRT, IOAPIC), the handler the central
// interrupt dispatcher calls, SVGA_REG_IRQMASK, and the wait channels the
// interrupt wakes.
//
// The mask holds the sources that stay armed for the life of the device
// (SVGA_IRQ_BASE_MASK), the ones armed by the legacy fence paths until the
// next reset, and the ones armed by refcounted waiters: the generic family
// (vmsvga2_irq_generic_waiter_add/_remove, one count per SVGA_IRQFLAG_* bit),
// the fence-sequence waiters (ANY_FENCE), the fence-goal waiters (FENCE_GOAL,
// or REG_FENCE_GOAL on a device with the extra register block) and the
// FIFO-progress waiters.  Only this file writes the mask register.
//
// The handler is split in two.  The first half reads and acknowledges the
// status, masks it by what is armed, and wakes whoever waits for FIFO space;
// the second half collects fence progress and hands the rest to the
// display-manager driver's callback.  Both run in interrupt context -- there
// is no interrupt thread here -- so neither sleeps: every wake is a
// wait-queue wake, which only makes tasks runnable.
//
// Without SVGA_CAP_IRQMASK (QEMU) or without a routed line nothing is ever
// armed; svga_irq_wait_event() then polls.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2009-2015 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include "vmsvga2_priv.h"
#include <kernel/hal/acpi.h>
#include <kernel/hal/ioapic.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/syscall.h> /* errno values */

/* The display-manager's fence goal (vmsvga2_hw_set_fence_goal()) keeps the
 * fence interrupt sources armed through a counted hold that is released as
 * soon as the device has passed the goal -- by the interrupt that says so,
 * or by the goal call itself when the fence had already gone by.  0 restores
 * the older behaviour: arm ANY_FENCE|FENCE_GOAL and leave them armed until
 * the next device reset, so every fence the device passes afterwards raises
 * an interrupt whether anybody waits for it or not. */
#ifndef VMSVGA2_IRQ_GOAL_HOLD
#define VMSVGA2_IRQ_GOAL_HOLD 1
#endif

/* A fence value counts as passed when the device's last-passed value is at
 * most this far beyond it.  The window has to be wide enough for every fence
 * that can be in flight at once and narrow enough that a value which has not
 * been emitted yet is never taken for an old one. */
#define SVGA_FENCE_WRAP (1U << 24)

/* How long a waiter with an armed interrupt sleeps before it looks again on
 * its own: the interrupt is the wake-up, this is only the net under it (a
 * host that coalesces or drops a status bit must not hang a waiter). */
#define SVGA_IRQ_SAFETY_TICKS 2

/* Device errors are reported at most this often. */
#define SVGA_IRQ_ERR_REPORT_US 5000000ULL

volatile int g_vmsvga_legacy_irq = -1;

// The line svga_irq_init() routed, kept when svga_irq_uninstall() stops
// claiming it, so a resume claims the same one again (svga_irq_reinstall()).
// -1: never routed (no SVGA_CAP_IRQMASK, or no usable route): polling.
static int g_svga_irq_line = -1;

spinlock_t svga_irq_lock = SPINLOCK_INIT("svga_irq");

// Woken by the interrupt when a fence passes (ANY_FENCE or the goal) ...
struct wait_queue_head g_svga_fence_wq = {
	.first = NULL,
	.lock = SPINLOCK_INIT("svga_fence_wq"),
};

// ...and when the host has consumed FIFO commands (FIFO_PROGRESS).
struct wait_queue_head g_svga_fifo_wq = {
	.first = NULL,
	.lock = SPINLOCK_INIT("svga_fifo_wq"),
};

// ===========================================================================
// Status register
// ===========================================================================

// The status lives in an I/O port on this device (the PCI function this
// driver binds, with its register file behind BAR0); the later
// memory-mapped generation keeps it in SVGA_REG_IRQ_STATUS instead.  The
// port is write-1-to-clear.
static inline uint32_t svga_irq_status_read(void)
{
	return inl(g_svga.io_base + SVGA_IRQSTATUS_PORT);
}

static inline void svga_irq_status_write(uint32_t status)
{
	outl(g_svga.io_base + SVGA_IRQSTATUS_PORT, status);
}

// Acknowledge exactly what is latched: whatever the device raised while
// nothing was listening, so it does not fire the moment its source is
// unmasked.  Before the interrupt is routed, and after every reset.
void svga_irq_preinstall(void)
{
	uint32_t status;

	if (!g_svga.present || !(g_svga.caps & SVGA_CAP_IRQMASK))
		return;
	status = svga_irq_status_read();
	svga_irq_status_write(status);
}

// ===========================================================================
// Interrupt mask
// ===========================================================================

// Serializes the waiter counts, the fence goal and every write of
// SVGA_REG_IRQMASK, so the value in the register is always the one the
// counts describe.  Nests outside svga_reg_lock (taken by svga_write_reg),
// never inside it.  Taken with interrupts off: the handler takes it too.
static spinlock_t svga_irqmask_lock = SPINLOCK_INIT("svga_irqmask");

#define SVGA_IRQ_WAITER_FLAGS (SVGA_IRQFLAG_MAX - 1)

static int g_irq_waiters[32]; // per SVGA_IRQFLAG_* bit
static uint32_t g_irq_waited; // bits with a waiter count > 0
static uint32_t g_irq_until_reset; // armed by the legacy fence paths
// The value last written to SVGA_REG_IRQMASK.  Read without the lock by the
// handler, which only uses it to filter the status it has just read.
static volatile uint32_t g_irq_mask;

// The fence goal: the value in SVGA_FIFO_FENCE_GOAL and whether it is still
// outstanding.  The goal hold (VMSVGA2_IRQ_GOAL_HOLD) is the refcounted
// waiter vmsvga2_hw_set_fence_goal() keeps while it is.
static uint32_t g_goal_seq;
static int g_goal_valid;
static int g_goal_held;

// Caller holds svga_irqmask_lock.
static void svga_irqmask_write_locked(void)
{
	uint32_t mask = SVGA_IRQ_BASE_MASK | g_irq_until_reset | g_irq_waited;

	g_irq_mask = mask;
	svga_write_reg(SVGA_REG_IRQMASK, mask);
}

// The goal interrupt the device raises: with the extra register block
// (SVGA_CAP2_EXTRA_REGS, in SVGA_REG_CAP2 as read at probe) it has a source
// of its own.
uint32_t svga_irq_fence_goal_flag(void)
{
	if ((svga_device_caps2() & SVGA_CAP2_EXTRA_REGS) != 0)
		return SVGA_IRQFLAG_REG_FENCE_GOAL;
	return SVGA_IRQFLAG_FENCE_GOAL;
}

uint32_t vmsvga2_irq_fence_goal_flag(void)
{
	return svga_irq_fence_goal_flag();
}

// Every source a fence passing can raise.
static uint32_t svga_irq_fence_flags(void)
{
	return SVGA_IRQFLAG_ANY_FENCE | SVGA_IRQFLAG_FENCE_GOAL |
	       SVGA_IRQFLAG_REG_FENCE_GOAL;
}

// Has the device passed fence `seq'?  Read straight from the FIFO register;
// no lock, any context.
static int svga_irq_seq_passed(uint32_t seq)
{
	if (!svga_has_fence())
		return 1;
	return (uint32_t)(g_svga.fifo[SVGA_FIFO_FENCE] - seq) < SVGA_FENCE_WRAP;
}

// Caller holds svga_irqmask_lock.  Returns true when the add was the first
// for any of the bits, i.e. the mask register was (or, with the interrupt
// not set up, would have been) reprogrammed: the caller's condition may have
// changed between its last look and the unmasking, so it looks again.
static bool svga_waiter_add_locked(uint32_t flag)
{
	uint32_t newly = 0;
	int bit;

	for (bit = 0; bit < 32; bit++) {
		if (!(flag & (1U << bit)))
			continue;
		if (g_irq_waiters[bit]++ == 0)
			newly |= 1U << bit;
	}
	if (!newly)
		return false;
	g_irq_waited |= newly;
	if (g_vmsvga_initialized && g_svga.irq_enabled) {
		// A status bit latched while the source was masked says
		// nothing about this waiter's condition, which it re-checks
		// after arming; acknowledge it so it does not fire as soon as
		// the source is unmasked.
		svga_irq_status_write(newly);
		svga_irqmask_write_locked();
	}
	return true;
}

// Caller holds svga_irqmask_lock.  True when the last waiter of any of the
// bits left and the source was masked again.
static bool svga_waiter_remove_locked(uint32_t flag)
{
	uint32_t cleared = 0;
	int bit;

	for (bit = 0; bit < 32; bit++) {
		if (!(flag & (1U << bit)))
			continue;
		if (WARN_ON_ONCE(g_irq_waiters[bit] == 0))
			continue; // unbalanced remove
		if (--g_irq_waiters[bit] == 0)
			cleared |= 1U << bit;
	}
	if (!cleared)
		return false;
	g_irq_waited &= ~cleared;
	if (g_vmsvga_initialized && g_svga.irq_enabled)
		svga_irqmask_write_locked();
	return true;
}

static uint32_t svga_waiter_flag_check(uint32_t flag)
{
	if (WARN_ON_ONCE(flag & ~SVGA_IRQ_WAITER_FLAGS))
		flag &= SVGA_IRQ_WAITER_FLAGS;
	return flag;
}

bool vmsvga2_irq_generic_waiter_add(uint32_t flag)
{
	bool programmed;
	uint64_t f;

	flag = svga_waiter_flag_check(flag);
	if (!flag)
		return false;
	spin_lock_irqsave(&svga_irqmask_lock, &f);
	programmed = svga_waiter_add_locked(flag);
	spin_unlock_irqrestore(&svga_irqmask_lock, f);
	return programmed;
}

bool vmsvga2_irq_generic_waiter_remove(uint32_t flag)
{
	bool programmed;
	uint64_t f;

	flag = svga_waiter_flag_check(flag);
	if (!flag)
		return false;
	spin_lock_irqsave(&svga_irqmask_lock, &f);
	programmed = svga_waiter_remove_locked(flag);
	spin_unlock_irqrestore(&svga_irqmask_lock, f);
	return programmed;
}

void vmsvga2_irq_waiter_add(uint32_t flag)
{
	(void)vmsvga2_irq_generic_waiter_add(flag);
}

void vmsvga2_irq_waiter_remove(uint32_t flag)
{
	(void)vmsvga2_irq_generic_waiter_remove(flag);
}

// A waiter for "the device's fence register moved": ANY_FENCE.
bool vmsvga2_irq_seqno_waiter_add(void)
{
	return vmsvga2_irq_generic_waiter_add(SVGA_IRQFLAG_ANY_FENCE);
}

bool vmsvga2_irq_seqno_waiter_remove(void)
{
	return vmsvga2_irq_generic_waiter_remove(SVGA_IRQFLAG_ANY_FENCE);
}

// A waiter for "the fence register reached the goal".
bool vmsvga2_irq_goal_waiter_add(void)
{
	return vmsvga2_irq_generic_waiter_add(svga_irq_fence_goal_flag());
}

bool vmsvga2_irq_goal_waiter_remove(void)
{
	return vmsvga2_irq_generic_waiter_remove(svga_irq_fence_goal_flag());
}

// A waiter for "the host consumed FIFO commands": FIFO_PROGRESS.
bool svga_fifo_waiter_add(void)
{
	return vmsvga2_irq_generic_waiter_add(SVGA_IRQFLAG_FIFO_PROGRESS);
}

bool svga_fifo_waiter_remove(void)
{
	return vmsvga2_irq_generic_waiter_remove(SVGA_IRQFLAG_FIFO_PROGRESS);
}

// Clear any stale status and program the mask from scratch: the sources
// that are always on, plus whatever refcounted waiters hold.  The sources
// armed "until reset" are dropped -- their waiters re-arm on their next
// wait.  The goal is forgotten (the device's fence register may have been
// reset under it), so the next goal is written whatever its value.
static void svga_irq_program_base(void)
{
	uint64_t f;

	svga_irq_preinstall();
	spin_lock_irqsave(&svga_irqmask_lock, &f);
	g_irq_until_reset = 0;
	g_goal_valid = 0;
	svga_irqmask_write_locked();
	spin_unlock_irqrestore(&svga_irqmask_lock, f);
}

/* After a device reset.  Re-arm rather than silence: a mode set happens
 * whenever the screen is resized, and leaving the mask at zero afterwards
 * turned the fence interrupt off for the rest of the session. */
void svga_irq_rearm(void)
{
	svga_irq_program_base();
	svga_irq_report_errors();
}

// Arm `flags' in addition to the base sources, and keep them armed until
// the next device reset.  What the FIFO fence wait and the display-manager's
// fence goal have always done; no counting.
void svga_irq_arm_until_reset(uint32_t flags)
{
	uint64_t f;

	spin_lock_irqsave(&svga_irqmask_lock, &f);
	g_irq_until_reset |= flags;
	if (g_svga.irq_enabled)
		svga_irqmask_write_locked();
	spin_unlock_irqrestore(&svga_irqmask_lock, f);
}

// Mask every source and acknowledge what is latched; the interrupt is no
// longer this driver's to claim afterwards.  The waiter counts are kept
// (svga_irq_reinstall() programs the mask from them again).  For suspend
// and shutdown.
void svga_irq_uninstall(void)
{
	uint64_t f;

	if (!g_svga.present || !(g_svga.caps & SVGA_CAP_IRQMASK))
		return;

	spin_lock_irqsave(&svga_irqmask_lock, &f);
	g_svga.irq_enabled = 0;
	g_irq_mask = 0;
	svga_write_reg(SVGA_REG_IRQMASK, 0);
	spin_unlock_irqrestore(&svga_irqmask_lock, f);

	svga_irq_preinstall();
	g_vmsvga_legacy_irq = -1;

	// Nobody is woken by the interrupt any more: send everyone parked on
	// it back to look for themselves (they fall back to polling).
	wq_wake_all(&g_svga_fence_wq);
	wq_wake_all(&g_svga_fifo_wq);
}

// The device can assert its pin: INTx not disabled in the PCI command
// register (a resume may find it set again).
static void svga_irq_intx_enable(void)
{
	const pci_device_t *dev = g_svga.pci;
	uint32_t cmd = pci_cfg_read32(dev->bus, dev->device, dev->function,
				      0x04);

	if (cmd & PCI_CMD_INTX_DISABLE)
		pci_cfg_write32(dev->bus, dev->device, dev->function, 0x04,
				cmd & ~PCI_CMD_INTX_DISABLE);
}

static void svga_goal_hold_update(void);

/* The other half of svga_irq_uninstall(), for a resume: the routing is the
 * one made at probe (the IOAPIC entry was never touched), so this only has
 * to make the device a source again.  The latched status is acknowledged
 * first, then the line is claimed, and only then is the mask programmed --
 * from the waiter counts, which the gap left alone -- so an interrupt the
 * newly unmasked sources raise always finds a handler.  A goal hold whose
 * goal the device passed while nothing listened (the fence register is
 * seeded on resume) is let go here: no interrupt will come for it.  Waiters
 * parked in svga_irq_wait_event() have been polling since the uninstall;
 * they are woken to look once more and take the interrupt again on their
 * next wait. */
void svga_irq_reinstall(void)
{
	uint64_t f;

	if (!g_svga.present || !(g_svga.caps & SVGA_CAP_IRQMASK) ||
	    g_svga_irq_line < 0 || g_svga.irq_enabled)
		return;

	svga_irq_intx_enable();
	svga_irq_preinstall();
	g_vmsvga_legacy_irq = g_svga_irq_line;
	spin_lock_irqsave(&svga_irqmask_lock, &f);
	g_svga.irq_enabled = 1;
	spin_unlock_irqrestore(&svga_irqmask_lock, f);
	svga_irq_rearm();
	svga_goal_hold_update();
	wq_wake_all(&g_svga_fence_wq);
	wq_wake_all(&g_svga_fifo_wq);
}

// ===========================================================================
// Fence goal
// ===========================================================================

// Caller holds svga_irqmask_lock.  Move the goal to `seq' unless a goal that
// is still outstanding lies beyond it: with two writers (the FIFO fence wait
// and the display-manager's fence emission) racing, a goal must never be
// pulled back to a fence that passes first and leave the later one without
// its interrupt.
static void svga_goal_write_locked(uint32_t seq)
{
	if (g_goal_valid && !svga_irq_seq_passed(g_goal_seq) &&
	    (uint32_t)(g_goal_seq - seq) < SVGA_FENCE_WRAP)
		return; // the outstanding goal is at or beyond `seq'
	g_goal_seq = seq;
	g_goal_valid = 1;
	g_svga.fifo[SVGA_FIFO_FENCE_GOAL] = seq;
}

void svga_irq_fence_goal_set(uint32_t seq)
{
	uint64_t f;

	if (!svga_has_fifo_reg(SVGA_FIFO_FENCE_GOAL))
		return;
	spin_lock_irqsave(&svga_irqmask_lock, &f);
	svga_goal_write_locked(seq);
	spin_unlock_irqrestore(&svga_irqmask_lock, f);
}

// Release the goal hold once the device has passed the goal.  Any context.
static void svga_goal_hold_update(void)
{
	uint64_t f;

	if (!g_goal_held)
		return; // racy peek; the decision is made under the lock
	spin_lock_irqsave(&svga_irqmask_lock, &f);
	if (g_goal_held && svga_irq_seq_passed(g_goal_seq)) {
		g_goal_held = 0;
		g_goal_valid = 0;
		svga_waiter_remove_locked(SVGA_IRQFLAG_ANY_FENCE |
					  svga_irq_fence_goal_flag());
	}
	spin_unlock_irqrestore(&svga_irqmask_lock, f);
}

void vmsvga2_hw_set_fence_goal(uint32_t goal)
{
	if (!g_svga.irq_enabled || !svga_has_fifo_reg(SVGA_FIFO_FENCE_GOAL))
		return;
#if VMSVGA2_IRQ_GOAL_HOLD
	{
		uint64_t f;
		int passed;

		// Already behind the device: nothing to arm for, and no
		// interrupt would come for it.
		if (svga_irq_seq_passed(goal))
			return;

		spin_lock_irqsave(&svga_irqmask_lock, &f);
		svga_goal_write_locked(goal);
		if (!g_goal_held) {
			g_goal_held = 1;
			svga_waiter_add_locked(SVGA_IRQFLAG_ANY_FENCE |
					       svga_irq_fence_goal_flag());
		}
		// Looked at again with the sources unmasked and the latched
		// status acknowledged: a fence that went by in between raised
		// nothing anyone will see, so the hold is dropped here instead
		// of by the interrupt.
		passed = svga_irq_seq_passed(g_goal_seq);
		if (passed) {
			g_goal_held = 0;
			g_goal_valid = 0;
			svga_waiter_remove_locked(SVGA_IRQFLAG_ANY_FENCE |
						  svga_irq_fence_goal_flag());
		}
		spin_unlock_irqrestore(&svga_irqmask_lock, f);
		if (!passed)
			svga_doorbell();
	}
#else
	g_svga.fifo[SVGA_FIFO_FENCE_GOAL] = goal;
	svga_irq_arm_until_reset(SVGA_IRQFLAG_ANY_FENCE |
				 SVGA_IRQFLAG_FENCE_GOAL);
	svga_doorbell();
#endif
}

// ===========================================================================
// Device errors
// ===========================================================================

// Counted in the handler, reported from process context: the interrupt may
// arrive while this processor holds the console lock, and printing there
// would deadlock on it.
static volatile uint32_t g_irq_err_events; // SVGA_IRQFLAG_ERROR raised
static volatile uint32_t g_irq_err_cmdbuf; // ...with a command-buffer completion
static uint32_t g_irq_err_reported;
static uint64_t g_irq_err_report_us;
static spinlock_t svga_irq_err_lock = SPINLOCK_INIT("svga_irq_err");

uint32_t vmsvga2_irq_error_count(void)
{
	return __atomic_load_n(&g_irq_err_events, __ATOMIC_RELAXED);
}

// Process context with interrupts on.  Rate-limited.
void svga_irq_report_errors(void)
{
	uint32_t n = __atomic_load_n(&g_irq_err_events, __ATOMIC_RELAXED);
	uint32_t cb = __atomic_load_n(&g_irq_err_cmdbuf, __ATOMIC_RELAXED);
	uint64_t now, f;
	uint32_t delta;

	if (n == g_irq_err_reported)
		return;
	now = timer_get_precise_us();
	spin_lock_irqsave(&svga_irq_err_lock, &f);
	if (n == g_irq_err_reported ||
	    (g_irq_err_report_us &&
	     now - g_irq_err_report_us < SVGA_IRQ_ERR_REPORT_US)) {
		spin_unlock_irqrestore(&svga_irq_err_lock, f);
		return;
	}
	delta = n - g_irq_err_reported;
	g_irq_err_reported = n;
	g_irq_err_report_us = now;
	spin_unlock_irqrestore(&svga_irq_err_lock, f);

	kprintf("svga2: device raised %u error interrupt%s (%u total, %u "
		"with a command-buffer completion)\n",
		delta, delta == 1 ? "" : "s", n, cb);
}

// ===========================================================================
// IRQ handling
// ===========================================================================

/* The fence interrupt: one consumer beyond this driver. */
static vmsvga2_hw_irq_cb_t g_hw_irq_cb;

void vmsvga2_hw_set_irq_callback(vmsvga2_hw_irq_cb_t cb)
{
	g_hw_irq_cb = cb;
}

// Work the first half hands to the second.
#define SVGA_IRQWORK_FENCE (1U << 0)
#define SVGA_IRQWORK_CMDBUF (1U << 1)

// The second half: fence progress, and the display-manager driver's part.
// Interrupt context; must not sleep.
static void svga_irq_thread_fn(uint32_t work, uint32_t status)
{
	vmsvga2_hw_irq_cb_t cb;

	if (work & SVGA_IRQWORK_FENCE) {
		svga_goal_hold_update();
		wq_wake_all(&g_svga_fence_wq);
	}

	// Command-buffer completions and errors are collected by the
	// command-buffer owner, which runs from the callback; it also sweeps
	// its fences there, so it is called for every interrupt the device
	// raised, with the raw status, as it always has been
	// (SVGA_IRQWORK_CMDBUF marks the interrupts that are its business).
	cb = g_hw_irq_cb;
	if (cb)
		cb(status);
}

int vmsvga2_irq(void)
{
	uint32_t status, masked_status;
	uint32_t work = 0;
	uint64_t f;

	if (!g_vmsvga_initialized)
		return 0;

	// Read+ack: the IRQ status port is write-1-to-clear.  Only what is
	// armed is acted on; everything is acknowledged.
	status = svga_irq_status_read();
	masked_status = status & g_irq_mask;
	if (likely(status))
		svga_irq_status_write(status);
	if (!status)
		return 0; // shared line, not ours

	spin_lock_irqsave(&svga_irq_lock, &f);
	g_svga.irq_pending |= status;
	spin_unlock_irqrestore(&svga_irq_lock, f);

	if (masked_status & SVGA_IRQFLAG_FIFO_PROGRESS)
		wq_wake_all(&g_svga_fifo_wq);

	if (masked_status & svga_irq_fence_flags())
		work |= SVGA_IRQWORK_FENCE;

	if (masked_status &
	    (SVGA_IRQFLAG_COMMAND_BUFFER | SVGA_IRQFLAG_ERROR))
		work |= SVGA_IRQWORK_CMDBUF;

	if (masked_status & SVGA_IRQFLAG_ERROR) {
		__atomic_add_fetch(&g_irq_err_events, 1, __ATOMIC_RELAXED);
		if (masked_status & SVGA_IRQFLAG_COMMAND_BUFFER)
			__atomic_add_fetch(&g_irq_err_cmdbuf, 1,
					   __ATOMIC_RELAXED);
	}

	svga_irq_thread_fn(work, status);
	return 1;
}

// ===========================================================================
// Waiting
// ===========================================================================

// Sleep on `wq' until it is woken, `cond' holds, or the safety net (or the
// deadline, `remaining_us' away) runs out.  Process context, interrupts on.
//
// The order is what makes it lost-wakeup safe on more than one processor:
// on the queue first, then the identity, then TASK_BLOCKED, and only then
// the condition is looked at again.  A wake that lands after the look finds
// a BLOCKED task it can claim; one that landed before it is seen by the
// look.  If the look succeeds the task takes itself back with the same CAS
// every waker uses -- and if a waker won that race the task has been queued
// to run, so it goes through the scheduler once to consume that.
static void svga_irq_park(struct wait_queue_head *wq, int (*cond)(void *),
			  void *arg, uint64_t remaining_us)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;
	uint64_t ticks, fl;
	int done;

	ticks = timer_us_to_ticks(remaining_us) + 1;
	if (ticks > SVGA_IRQ_SAFETY_TICKS)
		ticks = SVGA_IRQ_SAFETY_TICKS;

	wq_entry_init(&we, cur);
	wq_add(wq, &we);
	fl = local_irq_save();
	cur->wait_channel = wq;
	cur->wakeup_tick = timer_ticks() + ticks;
	cur->state = TASK_BLOCKED;
	done = cond(arg);
	if (done) {
		task_state_t expected = TASK_BLOCKED;

		if (__atomic_compare_exchange_n(&cur->state, &expected,
						TASK_RUNNING, false,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			done = 2; // nobody claimed us: just carry on
	}
	local_irq_restore(fl);
	if (done != 2)
		sched_schedule();
	// Disarm: this deadline and channel belong to this wait only.
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	wq_remove(wq, &we);
}

int svga_irq_wait_event(struct wait_queue_head *wq, uint32_t irq_flags,
			int (*cond)(void *), void *arg, uint64_t timeout_us,
			unsigned int flags)
{
	task_t *cur = sched_current();
	int can_sleep = cur && !irqs_disabled();
	int use_irq;
	uint64_t start;
	int ret = 0;

	if (cond(arg))
		return 0;

	use_irq = can_sleep && irq_flags && wq && g_svga.irq_enabled;
	if (can_sleep)
		svga_irq_report_errors();
	if (use_irq)
		vmsvga2_irq_generic_waiter_add(irq_flags);

	start = timer_get_precise_us();
	for (;;) {
		uint64_t elapsed;

		if (cond(arg))
			break;
		elapsed = timer_get_precise_us() - start;
		if (elapsed >= timeout_us) {
			ret = -ETIMEDOUT;
			break;
		}
		if (can_sleep && (flags & SVGA_WAIT_INTERRUPTIBLE) &&
		    signal_pending(cur)) {
			ret = -EINTR;
			break;
		}
		if (use_irq && g_svga.irq_enabled) {
			svga_irq_park(wq, cond, arg, timeout_us - elapsed);
			continue;
		}
		// No interrupt to wait for: poll.  Reading BUSY makes hosts
		// that only drain on demand process the FIFO.
		if (flags & SVGA_WAIT_NUDGE)
			(void)svga_read_reg(SVGA_REG_BUSY);
		if (can_sleep)
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}

	if (use_irq)
		vmsvga2_irq_generic_waiter_remove(irq_flags);
	return ret;
}

// ===========================================================================
// Routing
// ===========================================================================

// Route the device's legacy INTx line through the IOAPIC.  Only used when
// the device exposes SVGA_CAP_IRQMASK (VMware/VirtualBox; QEMU does not).
void svga_irq_init(void)
{
	const pci_device_t *dev = g_svga.pci;
	uint8_t irq = 0xFF;
	uint32_t gsi = 0;
	uint8_t pin = dev->interrupt_pin;

	if (!(g_svga.caps & SVGA_CAP_IRQMASK))
		return;

	// Resolve the IOAPIC GSI from ACPI _PRT first; the PCI config-space
	// interrupt_line is the legacy 8259 value and may differ from the
	// GSI in APIC mode (see the NIC drivers for the full story).
	if (pin >= 1 && pin <= 4) {
		uint8_t acpi_pin = pin - 1;
		uint8_t lookup_dev = dev->device;
		uint8_t lookup_pin = acpi_pin;

		if (dev->bus != 0) {
			const pci_device_t *bridge =
				pci_find_bridge_for_bus(dev->bus);
			if (bridge) {
				lookup_pin = (acpi_pin + dev->device) % 4;
				lookup_dev = bridge->device;
			}
		}
		if (acpi_pci_lookup_irq("\\\\_SB_.PCI0", lookup_dev,
					lookup_pin, &gsi) == 0 &&
		    gsi <= 23)
			irq = (uint8_t)gsi;
	}
	if (irq == 0xFF) {
		irq = dev->interrupt_line;
		if (irq == 0xFF || irq > 23) {
			kprintf("svga2: no valid INTx route, using polling\n");
			return;
		}
	}

	// Make sure the device can actually assert its interrupt pin.
	svga_irq_intx_enable();

	/* Clear any stale status, then arm the sources that must always be
	 * on.  The fence-goal sources below are armed per wait; these are not
	 * optional -- they are how a command-buffer completion is announced. */
	svga_irq_program_base();

	g_vmsvga_legacy_irq = irq;
	g_svga_irq_line = irq;
	ioapic_configure_legacy_irq(irq, (uint8_t)(32 + irq),
				    IOAPIC_POLARITY_LOW, IOAPIC_TRIGGER_LEVEL);
	g_svga.irq_enabled = 1;
	kprintf("svga2: fence IRQ on GSI %u\n", irq);
}
