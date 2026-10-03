// LikeOS VMware SVGA II display driver -- the command FIFO and fences.
//
// The FIFO is a ring in device memory (BAR2): the driver produces commands at
// NEXT_CMD, the host consumes them up to STOP.  The ring starts after the
// FIFO register block, whose size the device states in SVGA_REG_MEM_REGS.
//
// Producing.  One producer at a time holds svga_fifo_lock, interrupts off,
// from the reservation to the commit -- the reservation may point straight
// into the ring.  Nobody ever WAITS with that lock held: when the ring has no
// room the lock is dropped, the waiter sleeps on the FIFO-progress interrupt
// (or polls, where it may not sleep), then takes the lock again and looks
// afresh.  A command goes into the ring in place only when the device
// supports SVGA_FIFO_CAP_RESERVE (it then knows a reservation is being
// filled); otherwise it is built in a bounce buffer and copied in a dword at
// a time, NEXT_CMD following every dword, so the host never sees anything it
// was not told about.  A reservation that would wrap the end of the ring is
// bounced too.
//
// Announcing.  A register write leaves the virtual machine, so the host is
// told about new work only when it is idle: SVGA_FIFO_BUSY is 0 while the
// host is asleep, the first producer to swing it to 1 writes SVGA_REG_SYNC,
// and the host clears it when it runs out of work.  A device without that
// register gets SVGA_REG_SYNC every time.
//
// Fences are SVGA_CMD_FENCE values the host writes back to SVGA_FIFO_FENCE
// as it passes them.
//
// Also the FIFO and fence exports of vmsvga2_hw.h.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2009-2023 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright 2009-2015 VMware, Inc., Palo Alto, CA., USA

#include "vmsvga2_priv.h"
#include <kernel/ke/timer.h>
#include <kernel/ke/syscall.h> // errno values
#include <kernel/ke/smp.h>
#include <kernel/mm/memory.h>

/* The FIFO engine as it was before the wait/announce rework: the ring
 * starting right after the fixed register block, commands written in place
 * whether or not the device supports reservations, a doorbell on every
 * command, and a full ring waited for by spinning with svga_fifo_lock held
 * and interrupts off.  1 selects it again (bisecting a host that misbehaves
 * with the new engine); the fences and the exports are the same either way. */
#ifndef VMSVGA2_LEGACY_FIFO
#define VMSVGA2_LEGACY_FIFO 0
#endif

/* Map the FIFO write-back instead of uncached.  Off: no memory-management
 * helper hands out a cacheable device mapping, so this remaps the direct-map
 * alias by hand, and only when the FIFO lies inside the direct map.  The
 * memory-type range registers usually make the PCI hole uncached whatever
 * the page tables say, so the gain is uncertain. */
#ifndef VMSVGA2_FIFO_WB
#define VMSVGA2_FIFO_WB 0
#endif

/* Announce new work only to an idle host (the SVGA_FIFO_BUSY handshake).
 * 0: SVGA_REG_SYNC on every announcement, as the legacy engine does. */
#ifndef VMSVGA2_FIFO_LAZY_PING
#define VMSVGA2_FIFO_LAZY_PING 1
#endif

/* Treat a nonzero SVGA_FIFO_DEAD (with SVGA_FIFO_CAP_DEAD) as "the host has
 * given up on this device": FIFO, fence and drain waits then end at once
 * instead of running into their timeouts.  Off: the device headers do not
 * define what values the host writes there or when, and the display-manager
 * driver this one sits under never reads it -- a host that keeps something
 * else in that register would turn every fence wait into an instant
 * failure.  The value is logged at probe either way. */
#ifndef VMSVGA2_FIFO_DEAD_CHECK
#define VMSVGA2_FIFO_DEAD_CHECK 0
#endif

/* How long a producer waits for room in the ring before giving up on the
 * device -- and how long when it was called with interrupts off, where every
 * microsecond of waiting is a microsecond this processor takes no
 * interrupts.  A command the atomic caller cannot queue is dropped (counted,
 * reported later); a screen update is redrawn by the next one, a fence falls
 * back to a full drain. */
#define SVGA_FIFO_WAIT_US 3000000ULL
#define SVGA_FIFO_ATOMIC_WAIT_US 100000ULL

/* A fence value counts as passed when the device's last-passed value is at
 * most this far beyond it, and as stale -- signalled -- when it lies more
 * than this far behind the last value handed out (or beyond it: a value
 * never handed out has nothing to wait for). */
#define SVGA_FIFO_FENCE_WRAP (1U << 24)

spinlock_t svga_fifo_lock = SPINLOCK_INIT("svga_fifo");

static inline void svga_fifo_mb(void)
{
	__asm__ volatile("mfence" ::: "memory");
}

// ===========================================================================
// Shared helpers
// ===========================================================================

// The bounce buffer for a FIFO of `fifo_size' bytes.
//
// A reservation that wraps the end of the ring is handed out in the bounce
// buffer -- and so is every reservation on a device without
// SVGA_FIFO_CAP_RESERVE -- so the bounce has to hold the largest command
// anything may queue: commands up to half the FIFO are accepted (a single
// GMR2 page list, a 64x64 alpha cursor at 16408 bytes, the display-manager's
// 64 KiB batches), and a command larger than the bounce was written straight
// past its end.  The buffer is allocated once, at probe, because a
// reservation may be made with interrupts off -- including the console
// flush hook -- where nothing may allocate.  Capped at 1 MiB: a FIFO larger
// than 2 MiB accepts commands up to 1 MiB, which is still far beyond any
// single command this kernel builds.
uint32_t svga_fifo_bounce_size(uint32_t fifo_size)
{
	uint32_t size = fifo_size / 2;

	if (size < SVGA_FIFO_BOUNCE_MIN)
		size = SVGA_FIFO_BOUNCE_MIN;
	if (size > SVGA_FIFO_BOUNCE_MAX)
		size = SVGA_FIFO_BOUNCE_MAX;
	return size & ~3U;
}

// FIFO pointer sanity check — catches host/guest state corruption before it
// turns into wild writes.  Returns 0 when healthy.  Deliberately print-free:
// this runs inside the console flush hook (see err_* fields); failures are
// counted and reported later by svga_report_errors().
static int svga_fifo_sanity(void)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t min, max, next, stop;

	if (!fifo)
		return -1;
	min = fifo[SVGA_FIFO_MIN];
	max = fifo[SVGA_FIFO_MAX];
	next = fifo[SVGA_FIFO_NEXT_CMD];
	stop = fifo[SVGA_FIFO_STOP];

	if (unlikely(min < 4 * sizeof(uint32_t) || max <= min ||
		     max > g_svga.fifo_size || (max & 3) || (min & 3) ||
		     next < min || next >= max || stop < min || stop >= max ||
		     (next & 3) || (stop & 3))) {
		g_svga.err_fifo_corrupt++;
		return -1;
	}
	return 0;
}

// Has the host declared the device dead?  It does so through SVGA_FIFO_DEAD
// when it advertises SVGA_FIFO_CAP_DEAD; nothing it is sent is executed any
// more, so a wait for the ring to drain or a fence to pass would only run
// into its timeout.  Lock-free, any context.
static int svga_fifo_dead(void)
{
#if VMSVGA2_FIFO_DEAD_CHECK
	return svga_has_fifo_cap(SVGA_FIFO_CAP_DEAD) &&
	       svga_has_fifo_reg(SVGA_FIFO_DEAD) &&
	       g_svga.fifo[SVGA_FIFO_DEAD] != 0;
#else
	return 0;
#endif
}

// Report accumulated FIFO errors and attempt recovery.  Must only be called
// from contexts that may log and sleep (never from the console flush hook
// path, never from IRQ context).
void svga_report_errors(void)
{
	static volatile int in_recovery;
	uint32_t corrupt = g_svga.err_fifo_corrupt;
	uint32_t stall = g_svga.err_fifo_stall;
	uint32_t oversize = g_svga.err_fifo_oversize;
	uint32_t dead = g_svga.err_fifo_dead;
	uint32_t misuse = g_svga.err_fifo_misuse;
	int recover = 0;

	if (corrupt != g_svga.err_reported_corrupt) {
		g_svga.err_reported_corrupt = corrupt;
		WARN_RATELIMIT(1, "svga2: FIFO corruption detected (%u total)",
			       corrupt);
		recover = 1;
	}
	if (stall != g_svga.err_reported_stall) {
		g_svga.err_reported_stall = stall;
		WARN_RATELIMIT(1, "svga2: host FIFO stalls (%u total)", stall);
	}
	if (oversize != g_svga.err_reported_oversize) {
		g_svga.err_reported_oversize = oversize;
		WARN_RATELIMIT(1,
			       "svga2: FIFO commands above %u bytes refused "
			       "(%u total)",
			       g_svga.bounce_size, oversize);
	}
	if (dead != g_svga.err_reported_dead) {
		g_svga.err_reported_dead = dead;
		WARN_RATELIMIT(1,
			       "svga2: host reports the device dead "
			       "(SVGA_FIFO_DEAD=0x%x, %u waits abandoned)",
			       svga_has_fifo_reg(SVGA_FIFO_DEAD) ?
				       g_svga.fifo[SVGA_FIFO_DEAD] :
				       0,
			       dead);
	}
	if (misuse != g_svga.err_reported_misuse) {
		g_svga.err_reported_misuse = misuse;
		WARN_RATELIMIT(1,
			       "svga2: FIFO commit larger than its reservation "
			       "(%u total)",
			       misuse);
	}
	svga_irq_report_errors();
	if (recover && !in_recovery) {
		// FIFO state is toast: rebuild it (vmsvga2_reset re-enters
		// this function; the flag breaks the recursion).
		in_recovery = 1;
		if (vmsvga2_reset() == 0)
			kprintf("svga2: FIFO recovered by device reset\n");
		in_recovery = 0;
	}
}

// Tell the host there is work.  `reason' is SVGA_SYNC_GENERIC for new
// commands, SVGA_SYNC_FIFOFULL when the producer is waiting for room.
//
// With the SVGA_FIFO_BUSY register the host is told only when it is idle:
// the producer that swings BUSY from 0 to 1 rings, everyone after it finds 1
// and knows the host is already working, and will see NEXT_CMD move before
// it goes back to sleep and clears BUSY.  Without the register -- a device
// whose FIFO register block ends before it, or one without the extended
// FIFO -- nothing would ever clear it, so every announcement rings.
void svga_fifo_ping_host(uint32_t reason)
{
	volatile uint32_t *fifo = g_svga.fifo;

	if (!fifo)
		return;
#if VMSVGA2_FIFO_LAZY_PING && !VMSVGA2_LEGACY_FIFO
	if (g_svga.fifo_busy_reg) {
		uint32_t expected = 0;

		if (!__atomic_compare_exchange_n(&fifo[SVGA_FIFO_BUSY],
						 &expected, 1, false,
						 __ATOMIC_SEQ_CST,
						 __ATOMIC_SEQ_CST))
			return; // already busy: it will see the new work
	}
#endif
	svga_write_reg(SVGA_REG_SYNC, reason);
}

// Does the FIFO register block carry a pitch lock?  Read from the FIFO
// capabilities as they stand, not as they were at probe -- and only where
// the register block the device declared reaches the lock register, so a
// capability bit never leads to a write into the padding before the ring.
int svga_fifo_have_pitchlock(void)
{
	if (!(g_svga.caps & SVGA_CAP_EXTENDED_FIFO) || !g_svga.fifo)
		return 0;
	if (!svga_has_fifo_reg(SVGA_FIFO_PITCHLOCK))
		return 0;
	return (g_svga.fifo[SVGA_FIFO_CAPABILITIES] &
		SVGA_FIFO_CAP_PITCHLOCK) != 0;
}

#if VMSVGA2_LEGACY_FIFO
// ===========================================================================
// FIFO engine, legacy (svga_fifo_lock serializes reserve..commit)
// ===========================================================================

// Wait until the consumer makes progress; caller holds svga_fifo_lock.
// Returns 0 on progress, <0 on timeout (device wedged).
static int svga_fifo_wait_progress(void)
{
	uint64_t start = timer_get_precise_us();
	uint32_t stop0 = g_svga.fifo[SVGA_FIFO_STOP];

	svga_doorbell();
	for (;;) {
		// Reading BUSY forces synchronous FIFO processing on hosts
		// that only drain on demand.
		(void)svga_read_reg(SVGA_REG_BUSY);
		if (g_svga.fifo[SVGA_FIFO_STOP] != stop0)
			return 0;
		if (timer_get_precise_us() - start > SVGA_BUSY_TIMEOUT_US)
			return -1;
		__asm__ volatile("pause");
	}
}

// Reserve space for one command in the FIFO.  Returns a pointer the caller
// fills with `bytes` of command data, or NULL if the device is unusable.
// Caller must hold svga_fifo_lock and must call svga_fifo_commit() after.
// With `nowait' a full ring returns NULL at once instead of waiting for the
// host to drain it.
static void *svga_fifo_reserve_mode(uint32_t bytes, int nowait)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t min, max, next;

	BUG_ON(!spin_is_locked(&svga_fifo_lock));
	BUG_ON(bytes & 3); // commands are dword-granular

	// Suspended (or shut down): the device may be reset or its FIFO
	// stopped, and nothing queued now would ever run.  Refused, not
	// waited for.
	if (unlikely(g_svga.pm_suspended))
		return NULL;
	if (svga_fifo_sanity() != 0)
		return NULL;

	// The bounce buffer bounds every command, wrapping or not: a command
	// that only fits while the ring happens not to wrap would succeed or
	// fail by where the producer index stands.  Print-free (counted,
	// reported by svga_report_errors()).
	if (unlikely(bytes > g_svga.bounce_size || !g_svga.bounce)) {
		g_svga.err_fifo_oversize++;
		return NULL;
	}

	min = fifo[SVGA_FIFO_MIN];
	max = fifo[SVGA_FIFO_MAX];
	next = fifo[SVGA_FIFO_NEXT_CMD];

	if (unlikely(bytes > max - min - 4)) { // can never fit
		g_svga.err_fifo_oversize++;
		return NULL;
	}

	for (;;) {
		uint32_t stop = fifo[SVGA_FIFO_STOP];
		int fits_in_place = 0;
		int full = 0;

		if (next >= stop) {
			// Free region: [next, max) plus [min, stop)
			if (next + bytes < max ||
			    (next + bytes == max && stop > min))
				fits_in_place = 1;
			else if ((max - next) + (stop - min) < bytes + 4)
				full = 1;
			// else: fits, but wraps — use the bounce buffer
		} else {
			// Free region: [next, stop)
			if (next + bytes < stop)
				fits_in_place = 1;
			else
				full = 1;
		}

		if (full) {
			if (nowait)
				return NULL;
			if (svga_fifo_wait_progress() != 0) {
				// Print-free path: counted, reported later.
				g_svga.err_fifo_stall++;
				return NULL;
			}
			continue;
		}

		g_svga.reserved_bytes = bytes;
		if (fits_in_place && svga_has_fifo_cap(SVGA_FIFO_CAP_RESERVE))
			fifo[SVGA_FIFO_RESERVED] = bytes;
		if (fits_in_place) {
			g_svga.reserved_in_place = 1;
			return (void *)((uint8_t *)fifo + next);
		}
		g_svga.reserved_in_place = 0;
		return g_svga.bounce;
	}
}

void *svga_fifo_reserve(uint32_t bytes)
{
	return svga_fifo_reserve_mode(bytes, 0);
}

void *svga_fifo_reserve_nowait(uint32_t bytes)
{
	return svga_fifo_reserve_mode(bytes, 1);
}

// Publish a previously reserved command; caller holds svga_fifo_lock.
void svga_fifo_commit(uint32_t bytes)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t min = fifo[SVGA_FIFO_MIN];
	uint32_t max = fifo[SVGA_FIFO_MAX];
	uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];

	BUG_ON(!spin_is_locked(&svga_fifo_lock));
	if (unlikely(bytes > g_svga.reserved_bytes)) {
		g_svga.err_fifo_misuse++;
		bytes = g_svga.reserved_bytes;
	}

	if (!g_svga.reserved_in_place) {
		// Copy the bounced command into the ring dword by dword,
		// wrapping at max.
		const uint32_t *src = (const uint32_t *)g_svga.bounce;
		uint32_t i;

		for (i = 0; i < bytes / 4; i++) {
			fifo[next / 4] = src[i];
			next += 4;
			if (next == max)
				next = min;
		}
	} else {
		next += bytes;
		if (next >= max)
			next = min + (next - max);
	}

	__asm__ volatile("" ::: "memory"); // command data before NEXT_CMD
	fifo[SVGA_FIFO_NEXT_CMD] = next;
	if (svga_has_fifo_cap(SVGA_FIFO_CAP_RESERVE))
		fifo[SVGA_FIFO_RESERVED] = 0;
	g_svga.reserved_bytes = 0;
	g_svga.reserved_in_place = 0;
}

// ===========================================================================
// FIFO setup, legacy
// ===========================================================================

int svga_fifo_init(void)
{
	uint32_t min;
	size_t pages;
	uint64_t virt;

	g_svga.fifo_size = svga_read_reg(SVGA_REG_MEM_SIZE);
	g_svga.fifo_phys = svga_read_reg(SVGA_REG_MEM_START);
	if (g_svga.fifo_phys == 0 || g_svga.fifo_size < 0x10000) {
		WARN(1, "svga2: implausible FIFO phys=0x%lx size=%u",
		     (unsigned long)g_svga.fifo_phys, g_svga.fifo_size);
		return -1;
	}

	pages = (g_svga.fifo_size + PAGE_SIZE - 1) / PAGE_SIZE;
	virt = mm_map_device_mmio(g_svga.fifo_phys, pages);
	if (!virt)
		return -1;
	g_svga.fifo = (volatile uint32_t *)virt;

	// Command area starts after the FIFO register block.  With the
	// extended-FIFO capability the full register file is reserved so all
	// extended registers (fence, cursor bypass, 3D caps...) are valid.
	if (g_svga.caps & SVGA_CAP_EXTENDED_FIFO)
		min = SVGA_FIFO_NUM_REGS * sizeof(uint32_t);
	else
		min = 4 * sizeof(uint32_t);
	g_svga.fifo_min = min;
	g_svga.fifo_num_regs = min / sizeof(uint32_t);
	g_svga.fifo_busy_reg = 0;

	g_svga.fifo[SVGA_FIFO_MIN] = min;
	g_svga.fifo[SVGA_FIFO_MAX] = g_svga.fifo_size;
	g_svga.fifo[SVGA_FIFO_NEXT_CMD] = min;
	g_svga.fifo[SVGA_FIFO_STOP] = min;

	// Tell the device the FIFO is ready, then read the negotiated FIFO
	// capabilities (only defined after CONFIG_DONE on some hosts).
	svga_write_reg(SVGA_REG_CONFIG_DONE, 1);
	if (g_svga.caps & SVGA_CAP_EXTENDED_FIFO)
		g_svga.fifo_caps = g_svga.fifo[SVGA_FIFO_CAPABILITIES];
	else
		g_svga.fifo_caps = 0;
	return 0;
}

// Reset FIFO cursors after a device reset (the mapping is unchanged); the
// caller writes CONFIG_DONE afterwards.
void svga_fifo_reset_ring(void)
{
	uint64_t f;

	spin_lock_irqsave(&svga_fifo_lock, &f);
	{
		uint32_t min = g_svga.fifo_min;

		g_svga.fifo[SVGA_FIFO_MIN] = min;
		g_svga.fifo[SVGA_FIFO_MAX] = g_svga.fifo_size;
		g_svga.fifo[SVGA_FIFO_NEXT_CMD] = min;
		g_svga.fifo[SVGA_FIFO_STOP] = min;
		g_svga.reserved_bytes = 0;
		g_svga.reserved_in_place = 0;
	}
	spin_unlock_irqrestore(&svga_fifo_lock, f);
}

#else /* !VMSVGA2_LEGACY_FIFO */
// ===========================================================================
// FIFO engine
// ===========================================================================

// Not enough room for `bytes' in the ring right now?  Lock-free: the
// producer index only moves under svga_fifo_lock, and the host only ever
// makes more room.
static int svga_fifo_is_full(uint32_t bytes)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t max = fifo[SVGA_FIFO_MAX];
	uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
	uint32_t min = fifo[SVGA_FIFO_MIN];
	uint32_t stop = fifo[SVGA_FIFO_STOP];

	// One dword always stays free: NEXT_CMD == STOP means empty.
	if (next >= stop)
		return (max - next) + (stop - min) <= bytes;
	return stop - next <= bytes;
}

// The wait condition: room, or a host that will never make any.
static int svga_fifo_space_cond(void *arg)
{
	return !svga_fifo_is_full(*(const uint32_t *)arg) || svga_fifo_dead();
}

// Wait, WITHOUT svga_fifo_lock, until the ring has room for `bytes'.
//
// From a task with interrupts on this sleeps on the FIFO-progress interrupt
// (or yields between looks where the device has none); with interrupts off,
// or before the scheduler runs, it spins -- with the interrupt state the
// caller had, never one this layer imposed.  Returns 0 when there is room
// now, <0 when the host gave none within `timeout_us' or is dead.
static int svga_fifo_wait_space(uint32_t bytes, uint64_t timeout_us)
{
	int rc;

	if (!svga_fifo_is_full(bytes))
		return 0;
	svga_fifo_ping_host(SVGA_SYNC_FIFOFULL);
	rc = svga_irq_wait_event(&g_svga_fifo_wq, SVGA_IRQFLAG_FIFO_PROGRESS,
				 svga_fifo_space_cond, &bytes, timeout_us,
				 SVGA_WAIT_NUDGE);
	if (rc == 0 && svga_fifo_is_full(bytes) && svga_fifo_dead()) {
		// Ended by the host's death, not by room.  (Full but alive:
		// another producer took the room; the caller looks again.)
		g_svga.err_fifo_dead++;
		rc = -EIO;
	}
	return rc;
}

// One attempt at a reservation; caller holds svga_fifo_lock.  Returns the
// buffer to fill, or NULL with *full set when the ring has no room right now
// (wait and try again) and clear when the request is refused outright.
static void *svga_fifo_reserve_locked(uint32_t bytes, int *full)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t min, max, next, stop;
	int reserveable = svga_has_fifo_cap(SVGA_FIFO_CAP_RESERVE);
	int in_place = 0;

	BUG_ON(!spin_is_locked(&svga_fifo_lock));
	BUG_ON(bytes & 3); // commands are dword-granular
	*full = 0;

	// Suspended (or shut down): the device may be reset or its FIFO
	// stopped, and nothing queued now would ever run.  Refused outright,
	// so no producer waits for room that will not come.
	if (unlikely(g_svga.pm_suspended))
		return NULL;
	if (svga_fifo_sanity() != 0)
		return NULL;

	// The bounce buffer bounds every command, wrapping or not: a command
	// that only fits while the ring happens not to wrap would succeed or
	// fail by where the producer index stands.  Print-free (counted,
	// reported by svga_report_errors()).
	if (unlikely(bytes > g_svga.bounce_size || !g_svga.bounce)) {
		g_svga.err_fifo_oversize++;
		return NULL;
	}

	max = fifo[SVGA_FIFO_MAX];
	min = fifo[SVGA_FIFO_MIN];
	next = fifo[SVGA_FIFO_NEXT_CMD];
	if (unlikely(bytes >= max - min)) { // can never fit
		g_svga.err_fifo_oversize++;
		return NULL;
	}

	// A reservation left behind by a producer that never committed
	// (vmsvga2_hw_fifo_abort() clears it): taken over, not nested.
	if (unlikely(g_svga.reserved_bytes != 0)) {
		g_svga.err_fifo_misuse++;
		g_svga.reserved_bytes = 0;
	}

	stop = fifo[SVGA_FIFO_STOP];
	if (next >= stop) {
		// Free: [next, max) and [min, stop).
		if (likely(next + bytes < max ||
			   (next + bytes == max && stop > min)))
			in_place = 1;
		else if (svga_fifo_is_full(bytes)) {
			*full = 1;
			return NULL;
		}
		// else: fits, but wraps -- bounce.
	} else {
		// Free: [next, stop).
		if (likely(next + bytes < stop))
			in_place = 1;
		else {
			*full = 1;
			return NULL;
		}
	}

	g_svga.reserved_bytes = bytes;
	// In place only where the device knows a reservation is open; a
	// single dword is written atomically, so it needs no announcement.
	if (in_place && (reserveable || bytes <= sizeof(uint32_t))) {
		g_svga.reserved_in_place = 1;
		if (reserveable)
			fifo[SVGA_FIFO_RESERVED] = bytes;
		return (void *)((uint8_t *)fifo + next);
	}
	g_svga.reserved_in_place = 0;
	return g_svga.bounce;
}

// Reserve with svga_fifo_lock already held by the caller.  The lock is
// dropped while waiting for room and held again on return, whatever the
// result; another producer may have queued in between, which changes nothing
// for a caller that has not reserved yet.  The caller's interrupts are off
// (it took the lock with spin_lock_irqsave), so the wait spins, bounded by
// SVGA_FIFO_ATOMIC_WAIT_US: prefer svga_fifo_reserve_irqsave().
void *svga_fifo_reserve(uint32_t bytes)
{
	uint64_t timeout = irqs_enabled() ? SVGA_FIFO_WAIT_US :
					    SVGA_FIFO_ATOMIC_WAIT_US;
	uint64_t start = timer_get_precise_us();

	for (;;) {
		uint64_t elapsed;
		int full, rc;
		void *p = svga_fifo_reserve_locked(bytes, &full);

		if (p || !full)
			return p;
		elapsed = timer_get_precise_us() - start;
		if (elapsed >= timeout) {
			g_svga.err_fifo_stall++;
			return NULL;
		}
		spin_unlock(&svga_fifo_lock);
		rc = svga_fifo_wait_space(bytes, timeout - elapsed);
		spin_lock(&svga_fifo_lock);
		if (rc != 0) {
			g_svga.err_fifo_stall++;
			return NULL;
		}
	}
}

// The same without waiting: NULL at once when the ring is full.
void *svga_fifo_reserve_nowait(uint32_t bytes)
{
	int full;

	return svga_fifo_reserve_locked(bytes, &full);
}

// A bounced command into a device that supports reservations: announce the
// size, then copy it in at most two runs (the end of the ring, the start).
static void svga_fifo_res_copy(uint32_t next, uint32_t max, uint32_t min,
			       uint32_t bytes)
{
	volatile uint32_t *fifo = g_svga.fifo;
	const uint32_t *buffer = (const uint32_t *)g_svga.bounce;
	uint32_t chunk = max - next;
	uint32_t i;

	if (bytes < chunk)
		chunk = bytes;
	fifo[SVGA_FIFO_RESERVED] = bytes;
	svga_fifo_mb();
	for (i = 0; i < chunk / 4; i++)
		fifo[next / 4 + i] = buffer[i];
	for (i = 0; i < (bytes - chunk) / 4; i++)
		fifo[min / 4 + i] = buffer[chunk / 4 + i];
}

// A bounced command into a device without reservations: one dword at a
// time, the producer index following each, so the host never reads a dword
// it has not been handed.
static void svga_fifo_slow_copy(uint32_t next, uint32_t max, uint32_t min,
				uint32_t bytes)
{
	volatile uint32_t *fifo = g_svga.fifo;
	const uint32_t *buffer = (const uint32_t *)g_svga.bounce;

	while (bytes > 0) {
		fifo[next / 4] = *buffer++;
		next += sizeof(uint32_t);
		if (unlikely(next == max))
			next = min;
		svga_fifo_mb();
		fifo[SVGA_FIFO_NEXT_CMD] = next;
		svga_fifo_mb();
		bytes -= sizeof(uint32_t);
	}
}

// Publish a previously reserved command; caller holds svga_fifo_lock.  May
// publish less than was reserved (the caller built a shorter command), never
// more.  Does not announce it: see svga_fifo_ping_host().
void svga_fifo_commit(uint32_t bytes)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
	uint32_t max = fifo[SVGA_FIFO_MAX];
	uint32_t min = fifo[SVGA_FIFO_MIN];
	int reserveable = svga_has_fifo_cap(SVGA_FIFO_CAP_RESERVE);

	BUG_ON(!spin_is_locked(&svga_fifo_lock));
	BUG_ON(bytes & 3);
	// Print-free: the console flush may be waiting for this lock.
	if (unlikely(bytes > g_svga.reserved_bytes)) {
		g_svga.err_fifo_misuse++;
		bytes = g_svga.reserved_bytes;
	}
	g_svga.reserved_bytes = 0;

	if (!g_svga.reserved_in_place) {
		if (reserveable)
			svga_fifo_res_copy(next, max, min, bytes);
		else
			svga_fifo_slow_copy(next, max, min, bytes);
	}

	// The slow copy has already moved the index there, dword by dword;
	// writing the same value again is harmless.
	next += bytes;
	if (next >= max)
		next -= max - min;
	svga_fifo_mb(); // command data before NEXT_CMD
	fifo[SVGA_FIFO_NEXT_CMD] = next;
	if (reserveable)
		fifo[SVGA_FIFO_RESERVED] = 0;
	svga_fifo_mb();
	g_svga.reserved_in_place = 0;
}

// ===========================================================================
// FIFO setup
// ===========================================================================

// Map the FIFO.  Uncached unless VMSVGA2_FIFO_WB (see there).
static uint64_t svga_fifo_map(uint64_t phys, size_t pages)
{
#if VMSVGA2_FIFO_WB
	uint64_t base = phys & ~0xFFFULL;

	if (is_phys_in_direct_map(base + ((uint64_t)pages - 1) * PAGE_SIZE)) {
		uint64_t virt = (uint64_t)phys_to_virt(base);
		size_t i;

		for (i = 0; i < pages; i++) {
			if (!mm_map_page(virt + i * PAGE_SIZE,
					 base + i * PAGE_SIZE,
					 PAGE_PRESENT | PAGE_WRITABLE |
						 PAGE_GLOBAL |
						 PAGE_NO_EXECUTE))
				return 0;
		}
		if (sched_is_smp())
			smp_tlb_shootdown_sync();
		kprintf("svga2: FIFO mapped write-back\n");
		return virt + (phys & 0xFFFULL);
	}
	kprintf("svga2: FIFO outside the direct map, mapped uncached\n");
#endif
	return mm_map_device_mmio(phys, pages);
}

// Lay the ring out empty; caller holds svga_fifo_lock or is the only user.
static void svga_fifo_layout(void)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t min = g_svga.fifo_min;

	fifo[SVGA_FIFO_MIN] = min;
	fifo[SVGA_FIFO_MAX] = g_svga.fifo_size;
	svga_fifo_mb();
	fifo[SVGA_FIFO_NEXT_CMD] = min;
	fifo[SVGA_FIFO_STOP] = min;
	// The register area is at least a page, so BUSY is inside it even
	// where the device does not implement it.
	fifo[SVGA_FIFO_BUSY] = 0;
	if (svga_has_fifo_cap(SVGA_FIFO_CAP_RESERVE))
		fifo[SVGA_FIFO_RESERVED] = 0;
	svga_fifo_mb();
	g_svga.reserved_bytes = 0;
	g_svga.reserved_in_place = 0;
}

int svga_fifo_init(void)
{
	uint32_t regs = 4, mem_regs = 0, min, max;
	size_t pages;
	uint64_t virt;

	g_svga.fifo_size = svga_read_reg(SVGA_REG_MEM_SIZE);
	g_svga.fifo_phys = svga_read_reg(SVGA_REG_MEM_START);
	if (g_svga.fifo_phys == 0 || g_svga.fifo_size < 0x10000) {
		WARN(1, "svga2: implausible FIFO phys=0x%lx size=%u",
		     (unsigned long)g_svga.fifo_phys, g_svga.fifo_size);
		return -1;
	}

	pages = (g_svga.fifo_size + PAGE_SIZE - 1) / PAGE_SIZE;
	virt = svga_fifo_map(g_svga.fifo_phys, pages);
	if (!virt)
		return -1;
	g_svga.fifo = (volatile uint32_t *)virt;

	// The command area starts after the FIFO register block: four
	// registers, or with the extended FIFO as many as the device says it
	// has -- and never inside the first page.  A register count that does
	// not fit the FIFO is not believed; the extended block as this driver
	// knows it is assumed instead, which is what was always done.
	if (g_svga.caps & SVGA_CAP_EXTENDED_FIFO) {
		mem_regs = svga_read_reg(SVGA_REG_MEM_REGS);
		regs = mem_regs;
		if (regs == 0 || regs >= g_svga.fifo_size / 2 / sizeof(uint32_t)) {
			kprintf("svga2: FIFO register count %u implausible, "
				"assuming %u\n",
				mem_regs, (uint32_t)SVGA_FIFO_NUM_REGS);
			regs = SVGA_FIFO_NUM_REGS;
			mem_regs = 0;
		}
	}
	min = regs * sizeof(uint32_t);
	if (min < PAGE_SIZE)
		min = PAGE_SIZE;
	g_svga.fifo_min = min;
	g_svga.fifo_num_regs = regs;
	// The idle handshake needs a host that clears BUSY, i.e. one that
	// counts the register among its own.
	g_svga.fifo_busy_reg = mem_regs > SVGA_FIFO_BUSY;

	svga_fifo_layout();
	svga_write_reg(SVGA_REG_CONFIG_DONE, 1);

	// Read back what the device made of it; the capabilities are only
	// defined after CONFIG_DONE on some hosts.
	max = g_svga.fifo[SVGA_FIFO_MAX];
	min = g_svga.fifo[SVGA_FIFO_MIN];
	if (g_svga.caps & SVGA_CAP_EXTENDED_FIFO)
		g_svga.fifo_caps = g_svga.fifo[SVGA_FIFO_CAPABILITIES];
	else
		g_svga.fifo_caps = 0;
	kprintf("svga2: FIFO max 0x%08x min 0x%08x cap 0x%08x regs %u%s\n",
		max, min, g_svga.fifo_caps, regs,
		g_svga.fifo_busy_reg ? " (idle handshake)" : "");
	if (svga_has_fifo_cap(SVGA_FIFO_CAP_DEAD))
		kprintf("svga2: FIFO dead register 0x%x%s\n",
			g_svga.fifo[SVGA_FIFO_DEAD],
			VMSVGA2_FIFO_DEAD_CHECK ? " (checked)" : "");

	if (unlikely(min >= max)) {
		kprintf("svga2: FIFO memory is not usable\n");
		mm_unmap_mmio(virt, pages);
		g_svga.fifo = NULL;
		return -1;
	}
	return 0;
}

// Reset FIFO cursors after a device reset (the mapping is unchanged); the
// caller writes CONFIG_DONE afterwards.
void svga_fifo_reset_ring(void)
{
	uint64_t f;

	spin_lock_irqsave(&svga_fifo_lock, &f);
	svga_fifo_layout();
	spin_unlock_irqrestore(&svga_fifo_lock, f);
}

#endif /* VMSVGA2_LEGACY_FIFO */

// ===========================================================================
// Producing: lock, reserve, commit, announce
// ===========================================================================

// Take svga_fifo_lock and reserve `bytes'.  On success the lock is held
// (caller's interrupt state in *flags) until svga_fifo_commit_irqrestore()
// or svga_fifo_abort_irqrestore(); NULL means nothing is held.  Waits for
// room outside the lock: see svga_fifo_wait_space().
void *svga_fifo_reserve_irqsave(uint32_t bytes, uint64_t *flags)
{
#if VMSVGA2_LEGACY_FIFO
	void *p;

	spin_lock_irqsave(&svga_fifo_lock, flags);
	p = svga_fifo_reserve(bytes);
	if (!p)
		spin_unlock_irqrestore(&svga_fifo_lock, *flags);
	return p;
#else
	uint64_t timeout = irqs_enabled() ? SVGA_FIFO_WAIT_US :
					    SVGA_FIFO_ATOMIC_WAIT_US;
	uint64_t start = timer_get_precise_us();

	for (;;) {
		uint64_t elapsed;
		int full;
		void *p;

		spin_lock_irqsave(&svga_fifo_lock, flags);
		p = svga_fifo_reserve_locked(bytes, &full);
		if (p)
			return p;
		spin_unlock_irqrestore(&svga_fifo_lock, *flags);
		if (!full)
			return NULL;
		elapsed = timer_get_precise_us() - start;
		if (elapsed >= timeout ||
		    svga_fifo_wait_space(bytes, timeout - elapsed) != 0) {
			// Print-free path: counted, reported later.
			g_svga.err_fifo_stall++;
			return NULL;
		}
	}
#endif
}

// Why a non-blocking reservation came back empty.
#define SVGA_TRY_REFUSED 0 // refused outright: retrying will not help
#define SVGA_TRY_FULL 1 // the ring has no room right now
#define SVGA_TRY_CONTENDED 2 // another producer holds the lock

// The non-blocking form: NULL, with nothing held, when the lock is contended
// or the ring has no room for `bytes' right now (*why says which: try again
// later), or when the request is refused outright (SVGA_TRY_REFUSED).
static void *svga_fifo_try_reserve_mode(uint32_t bytes, uint64_t *flags,
					int *why)
{
	void *p;
	int full = 1;

	*flags = local_irq_save();
	if (!spin_trylock(&svga_fifo_lock)) {
		local_irq_restore(*flags);
		*why = SVGA_TRY_CONTENDED;
		return NULL;
	}
#if VMSVGA2_LEGACY_FIFO
	p = svga_fifo_reserve_nowait(bytes); // full or refused: retry
#else
	p = svga_fifo_reserve_locked(bytes, &full);
#endif
	if (!p)
		spin_unlock_irqrestore(&svga_fifo_lock, *flags);
	*why = full ? SVGA_TRY_FULL : SVGA_TRY_REFUSED;
	return p;
}

void *svga_fifo_try_reserve_irqsave(uint32_t bytes, uint64_t *flags)
{
	int why;

	return svga_fifo_try_reserve_mode(bytes, flags, &why);
}

// Commit, drop the lock, and (with `ping') tell the host.  Nothing else:
// the console's own sends use this, so they never re-enter the pending-box
// flush they are part of.
static void svga_fifo_commit_unlock(uint32_t bytes, uint64_t flags, int ping)
{
	svga_fifo_commit(bytes);
	spin_unlock_irqrestore(&svga_fifo_lock, flags);
	if (ping)
		svga_fifo_ping_host(SVGA_SYNC_GENERIC);
}

// ...and then hand the console the FIFO it may have found taken: a screen
// update that met this producer's lock is waiting in the console's pending
// box (svga_console_flush_pending(); a look at one flag when there is none).
void svga_fifo_commit_irqrestore(uint32_t bytes, uint64_t flags, int ping)
{
	svga_fifo_commit_unlock(bytes, flags, ping);
	(void)svga_console_flush_pending();
}

// Give a reservation back unused and drop the lock.
void svga_fifo_abort_irqrestore(uint64_t flags)
{
	if (g_svga.reserved_in_place &&
	    svga_has_fifo_cap(SVGA_FIFO_CAP_RESERVE))
		g_svga.fifo[SVGA_FIFO_RESERVED] = 0;
	g_svga.reserved_bytes = 0;
	g_svga.reserved_in_place = 0;
	spin_unlock_irqrestore(&svga_fifo_lock, flags);
	(void)svga_console_flush_pending();
}

// Convenience: reserve, copy, commit — one self-contained command, queued
// but not announced.  The doorbell is a register write and a register write
// leaves the virtual machine, so a caller with a run of commands to queue
// rings once at the end rather than once per command.
int svga_fifo_write_cmd_quiet(const void *cmd, uint32_t bytes)
{
	uint64_t f;
	void *dst;

	dst = svga_fifo_reserve_irqsave(bytes, &f);
	if (!dst)
		return -1;
	kmemcpy(dst, cmd, bytes);
	svga_fifo_commit_irqrestore(bytes, f, 0);
	return 0;
}

// ...and the announcing form, which is what a lone command uses.
int svga_fifo_write_cmd(const void *cmd, uint32_t bytes)
{
	int rc = svga_fifo_write_cmd_quiet(cmd, bytes);

	if (rc == 0)
		svga_fifo_ping_host(SVGA_SYNC_GENERIC);
	return rc;
}

// One command, announced, without ever waiting: for the console flush and
// anything else that runs where it must not.  -EAGAIN when the lock is
// contended or the ring is full right now (keep the work, retry later),
// -EIO when the command is refused outright or the device is absent.
//
// A full ring is announced (SVGA_SYNC_FIFOFULL) before giving up, so a host
// that has not been told about the work already queued starts on it and the
// retry finds room.  A contended lock is not: its holder announces what it
// commits.
int svga_fifo_try_write_cmd(const void *cmd, uint32_t bytes)
{
	uint64_t f;
	int why;
	void *dst;

	if (!g_svga.present || !g_svga.fifo || (bytes & 3) ||
	    bytes > g_svga.bounce_size)
		return -EIO;
	dst = svga_fifo_try_reserve_mode(bytes, &f, &why);
	if (!dst) {
		if (why == SVGA_TRY_REFUSED)
			return -EIO;
		if (why == SVGA_TRY_FULL)
			svga_fifo_ping_host(SVGA_SYNC_FIFOFULL);
		return -EAGAIN;
	}
	kmemcpy(dst, cmd, bytes);
	svga_fifo_commit_unlock(bytes, f, 1);
	return 0;
}

// ===========================================================================
// Fences and synchronization
// ===========================================================================

int svga_has_fence(void)
{
	return svga_has_fifo_cap(SVGA_FIFO_CAP_FENCE) &&
	       svga_has_fifo_reg(SVGA_FIFO_FENCE);
}

/* Does something above this layer own a command-buffer channel on the same
 * device?
 *
 * The device has ONE fence register, and an SVGA_CMD_FENCE executed out of a
 * command buffer advances it exactly as readily as one executed out of the
 * FIFO.  With two streams emitting fences into one register, a value on it
 * says "somebody got this far" -- which is not what a waiter asked.  A FIFO
 * wait then returns because a command-buffer fence went past it, and the
 * caller unbinds a guest memory region, or reuses a buffer, that the host has
 * not finished reading.
 *
 * So while a command-buffer owner is registered this layer emits NO fences,
 * and every drain here falls back to SVGA_REG_SYNC -- unambiguous, and what a
 * device without fence support uses anyway.  The register is then written by
 * one stream only, which is the invariant kept by never using the FIFO for
 * commands once a command-buffer manager exists. */
static int g_cmdbuf_owner;

/* Where that owner wants this layer's guest-memory-region commands sent.
 *
 * A screen object or a blit names a region by id, and on a device driven
 * through command buffers those commands go down the command-buffer
 * channel.  The DEFINE_GMR2/REMAP_GMR2 that make the id mean anything used
 * to go into the FIFO regardless -- two streams with no order between
 * them, and the host executed the screen definition before it had heard
 * of the region it named, and refused it (VMware, 3D off: "command error
 * at offset 0: command 34").  With the channel set, the region commands
 * travel the same way as everything that refers to them. */
int (*g_cmd_channel)(const void *cmds, uint32_t bytes, int ring);

void vmsvga2_set_cmdbuf_owner(int on)
{
	g_cmdbuf_owner = on ? 1 : 0;
}

void vmsvga2_set_cmd_channel(int (*submit)(const void *cmds, uint32_t bytes,
					    int ring))
{
	g_cmd_channel = submit;
}

// The next fence value; 0 is never one.  Caller holds svga_fifo_lock, so
// the values are monotonic in the order their commands enter the FIFO.
static uint32_t svga_fence_next_locked(void)
{
	uint32_t fence = ++g_svga.next_fence;

	if (fence == 0)
		fence = g_svga.next_fence = 1;
	return fence;
}

/* Allocate a fence number without submitting anything.
 *
 * The caller submits SVGA_CMD_FENCE itself, through the channel its other
 * commands went down.  That matters once command buffers carry the work:
 * a fence written to the FIFO orders against the FIFO, not against a
 * command-buffer context, so it can pass while the batches it was meant to
 * follow are still being executed. */
uint32_t vmsvga2_fence_alloc(void)
{
	uint32_t fence;
	uint64_t f;

	if (!g_svga.present || !svga_has_fence())
		return 0;
	spin_lock_irqsave(&svga_fifo_lock, &f);
	fence = svga_fence_next_locked();
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	return fence;
}

/* Queue SVGA_CMD_FENCE and announce it.  The number is taken only once the
 * command has room, so a fence that could not be queued burns no number --
 * every value handed out is one the host will write back.  0 means "no
 * fence for this stream" (none supported, a command-buffer owner holds the
 * fence register, or no room): the caller then drains with SVGA_REG_SYNC
 * (svga_legacy_sync), which covers everything queued so far. */
uint32_t vmsvga2_fence_insert(void)
{
	uint32_t fence;
	uint64_t f;
	uint32_t *cmd;

	/* See g_cmdbuf_owner. */
	if (!g_svga.present || !svga_has_fence() || g_cmdbuf_owner)
		return 0;

	cmd = svga_fifo_reserve_irqsave(2 * sizeof(uint32_t), &f);
	if (!cmd)
		return 0;
	fence = svga_fence_next_locked();
	cmd[0] = SVGA_CMD_FENCE;
	cmd[1] = fence;
	svga_fifo_commit_irqrestore(2 * sizeof(uint32_t), f, 1);
	return fence;
}

/* Has the device passed `fence'?  Passed when the device's last-passed value
 * is at most SVGA_FIFO_FENCE_WRAP beyond it; also -- stale -- when the value
 * lies further behind the last one handed out than that, or was never handed
 * out at all: nothing will ever write it back, and nothing waits for it. */
int vmsvga2_fence_passed(uint32_t fence)
{
	if (!g_svga.present || fence == 0 || !svga_has_fence())
		return 1;
	if (g_svga.fifo[SVGA_FIFO_FENCE] - fence < SVGA_FIFO_FENCE_WRAP)
		return 1;
	return g_svga.next_fence - fence > SVGA_FIFO_FENCE_WRAP;
}

// Wait conditions: lock-free, any context (svga_irq_wait_event() calls
// them with interrupts off at times).
static int svga_fence_cond(void *arg)
{
	return vmsvga2_fence_passed(*(const uint32_t *)arg) ||
	       svga_fifo_dead();
}

static int svga_idle_cond(void *arg)
{
	(void)arg;
	return svga_read_reg(SVGA_REG_BUSY) == 0 || svga_fifo_dead();
}

// Full drain: SVGA_REG_SYNC, then wait for SVGA_REG_BUSY to clear -- the
// device has executed everything queued before the SYNC.  Sleeps (yields
// between looks) where it may, spins otherwise.
static int svga_legacy_sync_now(void)
{
	int rc;

	svga_write_reg(SVGA_REG_SYNC, SVGA_SYNC_GENERIC);
	rc = svga_irq_wait_event(NULL, 0, svga_idle_cond, NULL,
				 SVGA_BUSY_TIMEOUT_US, 0);
	if (rc == 0 && svga_fifo_dead()) {
		g_svga.err_fifo_dead++; // ended by the host's death
		return -1;
	}
	if (rc != 0) {
		WARN_RATELIMIT(1, "svga2: SYNC/BUSY drain timed out");
		return -1;
	}
	return 0;
}

/* Wait until the device has passed `fence', for at most `timeout_us'.
 *
 * Not interruptible: the drains built on this (vmsvga2_fifo_flush before a
 * guest memory region is unbound, say) must not return early because the
 * caller has a signal pending -- a task being killed tears its regions down
 * with SIGKILL pending, and the host would then read memory already handed
 * back.  The wait is bounded by `timeout_us' all the same. */
int vmsvga2_fence_wait(uint32_t fence, uint64_t timeout_us)
{
	uint32_t irq_flags = 0;
	int rc;

	if (!g_svga.present)
		return 0;
	if (fence == 0 || !svga_has_fence())
		return svga_legacy_sync(); // no fence: wait for idle

	if (vmsvga2_fence_passed(fence))
		return 0;
	if (svga_fifo_dead()) {
		g_svga.err_fifo_dead++;
		return -1;
	}

	if (g_svga.irq_enabled && svga_has_fifo_reg(SVGA_FIFO_FENCE_GOAL)) {
		svga_irq_fence_goal_set(fence);
		irq_flags = SVGA_IRQFLAG_ANY_FENCE | svga_irq_fence_goal_flag();
	}
	// Unconditional: a goal the host has not been told about may never
	// be looked at.
	svga_doorbell();
	rc = svga_irq_wait_event(&g_svga_fence_wq, irq_flags, svga_fence_cond,
				 &fence, timeout_us, SVGA_WAIT_NUDGE);
	if (rc == 0 && !vmsvga2_fence_passed(fence) && svga_fifo_dead()) {
		g_svga.err_fifo_dead++; // ended by the host's death
		return -1;
	}
	if (rc != 0) {
		WARN_RATELIMIT(1, "svga2: fence %u wait timeout", fence);
		return -1;
	}
	return 0;
}

int svga_legacy_sync(void)
{
	// Suspended: the suspend drained the device, and nothing has been
	// queued since (the FIFO refuses); a reset device may not even
	// answer the way a running one does.
	if (g_svga.pm_suspended)
		return 0;
	return svga_legacy_sync_now();
}

/* Close the FIFO and drain it.  From here every reservation is refused
 * (g_svga.pm_suspended); a producer that already holds the lock -- it
 * reserved before the flag went up -- commits first, which taking and
 * dropping the lock waits out; then one SVGA_REG_SYNC/SVGA_REG_BUSY drain
 * covers whatever went in between the caller's last drain and the flag.
 * The caller drains with fences beforehand (vmsvga2_fifo_flush()) where it
 * can: this is the backstop.  Process context. */
int svga_fifo_quiesce(void)
{
	uint64_t f;

	g_svga.pm_suspended = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	spin_lock_irqsave(&svga_fifo_lock, &f);
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	return svga_legacy_sync_now();
}

void vmsvga2_fifo_flush(void)
{
	uint32_t fence;

	if (!g_svga.present)
		return;
	fence = vmsvga2_fence_insert();
	if (fence)
		(void)vmsvga2_fence_wait(fence, SVGA_FENCE_TIMEOUT_US);
	else
		(void)svga_legacy_sync();
	// The ring has drained: a screen update that found it full goes now.
	(void)svga_console_flush_pending();
}

// ===========================================================================
// FIFO and fence exports (vmsvga2_hw.h)
// ===========================================================================

uint32_t vmsvga2_hw_fifo_max_bytes(void)
{
	if (!g_svga.present || !g_svga.fifo || !g_svga.bounce)
		return 0;
	return g_svga.bounce_size;
}

/* One command of `bytes' from `data', atomically. */
int vmsvga2_hw_fifo_submit(const void *data, uint32_t bytes)
{
	if (!g_svga.present || !g_svga.fifo)
		return -1;
	if (bytes & 3)
		return -1;
	if (bytes > g_svga.bounce_size)
		return -1;
	return svga_fifo_write_cmd(data, bytes);
}

/* The same, queued but not announced: the caller has a run of commands and
 * finishes it with vmsvga2_hw_doorbell().  One trap out of the virtual
 * machine for the run instead of one per damage rectangle. */
int vmsvga2_hw_fifo_submit_batch(const void *data, uint32_t bytes)
{
	if (!g_svga.present || !g_svga.fifo)
		return -1;
	if (bytes & 3)
		return -1;
	if (bytes > g_svga.bounce_size)
		return -1;
	return svga_fifo_write_cmd_quiet(data, bytes);
}

/* A reservation the caller fills in place; must be followed by a commit
 * of the same size with the lock still held.  The lock is taken here and
 * released by the commit (or the abort).
 *
 * The saved interrupt state is stored only once the lock is held: saving
 * straight into the shared variable while still spinning for the lock let
 * a second reserver overwrite the holder's flags, and the holder's commit
 * then restored the wrong interrupt state. */
static uint64_t g_hw_fifo_flags;

void *vmsvga2_hw_fifo_reserve(uint32_t bytes)
{
	uint64_t f;
	void *dst;

	if (!g_svga.present || !g_svga.fifo || (bytes & 3))
		return NULL;
	if (bytes > g_svga.bounce_size)
		return NULL;
	dst = svga_fifo_reserve_irqsave(bytes, &f);
	if (!dst)
		return NULL;
	g_hw_fifo_flags = f;
	return dst;
}

void *vmsvga2_hw_fifo_try_reserve(uint32_t bytes)
{
	uint64_t f;
	void *dst;

	if (!g_svga.present || !g_svga.fifo || (bytes & 3))
		return NULL;
	if (bytes > g_svga.bounce_size)
		return NULL;
	dst = svga_fifo_try_reserve_irqsave(bytes, &f);
	if (!dst)
		return NULL;
	g_hw_fifo_flags = f;
	return dst;
}

void vmsvga2_hw_fifo_commit(uint32_t bytes)
{
	uint64_t f = g_hw_fifo_flags; // read while the lock still guards it

	svga_fifo_commit_irqrestore(bytes, f, 1);
}

void vmsvga2_hw_fifo_abort(void)
{
	uint64_t f = g_hw_fifo_flags;

	svga_fifo_abort_irqrestore(f);
}

void vmsvga2_hw_doorbell(void)
{
	svga_fifo_ping_host(SVGA_SYNC_GENERIC);
}

int vmsvga2_hw_fifo_have_pitchlock(void)
{
	return g_svga.present && svga_fifo_have_pitchlock();
}

uint32_t vmsvga2_hw_fence_current(void)
{
	return svga_has_fence() ? g_svga.fifo[SVGA_FIFO_FENCE] : 0;
}

int vmsvga2_hw_has_fence(void)
{
	return svga_has_fence();
}
