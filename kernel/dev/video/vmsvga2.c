// LikeOS-64 VMware SVGA II display driver
//
// Supports the SVGA II virtual display adapter exposed by VMware products,
// QEMU ("-vga vmware") and VirtualBox ("VMSVGA").  Every optional feature is
// strictly capability-gated so the driver degrades gracefully on hosts with
// reduced implementations; when the device is absent or bring-up fails the
// kernel keeps using the GOP framebuffer path unchanged.
//
// See include/kernel/dev/video/vmsvga2.h for the architecture and locking
// model overview.

#include <kernel/dev/video/vmsvga2.h>
#include <kernel/dev/video/fb.h>
#include <kernel/uapi/bug.h>
#include <kernel/io/console.h>
#include <kernel/hal/pci.h>
#include <kernel/hal/acpi.h>
#include <kernel/hal/ioapic.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/signal.h>
#include <kernel/mm/memory.h>

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

	// FIFO bounce buffer for reservations that wrap the ring
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
	uint32_t err_reported_corrupt;
	uint32_t err_reported_stall;

	// Fences
	uint32_t next_fence;

	// IRQ
	int irq_enabled; // SVGA_CAP_IRQMASK negotiated + routed
	uint32_t irq_pending; // accumulated SVGA_IRQFLAG_* (under irq_lock)

	int active; // display owned by this driver
	int present; // device probed successfully
} svga_state_t;

static svga_state_t g_svga;

volatile int g_vmsvga_initialized = 0;
volatile int g_vmsvga_legacy_irq = -1;

// Locks (see header for the locking model)
static spinlock_t svga_reg_lock = SPINLOCK_INIT("svga_reg");
static spinlock_t svga_fifo_lock = SPINLOCK_INIT("svga_fifo");
static spinlock_t svga_cursor_lock = SPINLOCK_INIT("svga_cursor");
static spinlock_t svga_irq_lock = SPINLOCK_INIT("svga_irq");
static svga_mutex_t svga_modeset_mutex;

// Wait channel key for fence waiters (IRQ mode)
static int svga_fence_waiters;

// ===========================================================================
// Register access (index/value port pair, serialized by svga_reg_lock)
// ===========================================================================

static uint32_t svga_read_reg(uint32_t index)
{
	uint64_t f;
	uint32_t val;

	spin_lock_irqsave(&svga_reg_lock, &f);
	outl(g_svga.io_base + SVGA_INDEX_PORT, index);
	val = inl(g_svga.io_base + SVGA_VALUE_PORT);
	spin_unlock_irqrestore(&svga_reg_lock, f);
	return val;
}

static void svga_write_reg(uint32_t index, uint32_t value)
{
	uint64_t f;

	spin_lock_irqsave(&svga_reg_lock, &f);
	outl(g_svga.io_base + SVGA_INDEX_PORT, index);
	outl(g_svga.io_base + SVGA_VALUE_PORT, value);
	spin_unlock_irqrestore(&svga_reg_lock, f);
}

// Ask the host to start processing the FIFO (asynchronous doorbell)
static void svga_doorbell(void)
{
	svga_write_reg(SVGA_REG_SYNC, 1);
}

static int svga_has_fifo_reg(uint32_t reg)
{
	// Extended FIFO registers are valid when the FIFO bookkeeping area
	// (fifo[MIN]) is large enough to contain them.
	return g_svga.fifo && g_svga.fifo[SVGA_FIFO_MIN] > reg * sizeof(uint32_t);
}

static int svga_has_fifo_cap(uint32_t cap)
{
	return (g_svga.fifo_caps & cap) != 0;
}

// ===========================================================================
// FIFO engine (producer side; svga_fifo_lock serializes reserve..commit)
// ===========================================================================

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

// Report accumulated FIFO errors and attempt recovery.  Must only be called
// from contexts that may log and sleep (never from the console flush hook
// path, never from IRQ context).
static void svga_report_errors(void)
{
	static volatile int in_recovery;
	uint32_t corrupt = g_svga.err_fifo_corrupt;
	uint32_t stall = g_svga.err_fifo_stall;
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
	if (recover && !in_recovery) {
		// FIFO state is toast: rebuild it (vmsvga2_reset re-enters
		// this function; the flag breaks the recursion).
		in_recovery = 1;
		if (vmsvga2_reset() == 0)
			kprintf("svga2: FIFO recovered by device reset\n");
		in_recovery = 0;
	}
}

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
static void *svga_fifo_reserve(uint32_t bytes)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t min, max, next;

	BUG_ON(!spin_is_locked(&svga_fifo_lock));
	BUG_ON(bytes & 3); // commands are dword-granular
	WARN_ON_ONCE(g_svga.reserved_bytes != 0); // nested reservation

	if (svga_fifo_sanity() != 0)
		return NULL;

	min = fifo[SVGA_FIFO_MIN];
	max = fifo[SVGA_FIFO_MAX];
	next = fifo[SVGA_FIFO_NEXT_CMD];

	if (WARN_ON_ONCE(bytes > max - min - 4)) // can never fit
		return NULL;

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

// Publish a previously reserved command; caller holds svga_fifo_lock.
static void svga_fifo_commit(uint32_t bytes)
{
	volatile uint32_t *fifo = g_svga.fifo;
	uint32_t min = fifo[SVGA_FIFO_MIN];
	uint32_t max = fifo[SVGA_FIFO_MAX];
	uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];

	BUG_ON(!spin_is_locked(&svga_fifo_lock));
	WARN_ON_ONCE(bytes != g_svga.reserved_bytes);

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

// Convenience: reserve, copy, commit — one self-contained command, queued
// but not announced.  The doorbell is a register write and a register write
// leaves the virtual machine, so a caller with a run of commands to queue
// rings once at the end rather than once per command.
static int svga_fifo_write_cmd_quiet(const void *cmd, uint32_t bytes)
{
	uint64_t f;
	void *dst;

	spin_lock_irqsave(&svga_fifo_lock, &f);
	dst = svga_fifo_reserve(bytes);
	if (!dst) {
		spin_unlock_irqrestore(&svga_fifo_lock, f);
		return -1;
	}
	kmemcpy(dst, cmd, bytes);
	svga_fifo_commit(bytes);
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	return 0;
}

// ...and the announcing form, which is what a lone command uses.
static int svga_fifo_write_cmd(const void *cmd, uint32_t bytes)
{
	int rc = svga_fifo_write_cmd_quiet(cmd, bytes);

	if (rc == 0)
		svga_doorbell();
	return rc;
}

// ===========================================================================
// Fences and synchronization
// ===========================================================================

static int svga_has_fence(void)
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
 * one stream only, which is the invariant the reference driver keeps by never
 * using the FIFO for commands once it has a command-buffer manager. */
static int g_cmdbuf_owner;

void vmsvga2_set_cmdbuf_owner(int on)
{
	g_cmdbuf_owner = on ? 1 : 0;
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
	fence = ++g_svga.next_fence;
	if (fence == 0)
		fence = g_svga.next_fence = 1;
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	return fence;
}

uint32_t vmsvga2_fence_insert(void)
{
	uint32_t fence;
	uint32_t cmd[2];
	uint64_t f;
	void *dst;

	/* 0 means "no fence for this stream": every caller then drains with
	 * SVGA_REG_SYNC instead.  See g_cmdbuf_owner. */
	if (!g_svga.present || !svga_has_fence() || g_cmdbuf_owner)
		return 0;

	spin_lock_irqsave(&svga_fifo_lock, &f);
	// Fence values are allocated under the FIFO lock so they are
	// monotonic in FIFO order; 0 is never a valid fence.
	fence = ++g_svga.next_fence;
	if (fence == 0)
		fence = g_svga.next_fence = 1;
	cmd[0] = SVGA_CMD_FENCE;
	cmd[1] = fence;
	dst = svga_fifo_reserve(sizeof(cmd));
	if (!dst) {
		spin_unlock_irqrestore(&svga_fifo_lock, f);
		return 0;
	}
	kmemcpy(dst, cmd, sizeof(cmd));
	svga_fifo_commit(sizeof(cmd));
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	svga_doorbell();
	return fence;
}

int vmsvga2_fence_passed(uint32_t fence)
{
	if (!g_svga.present || fence == 0 || !svga_has_fence())
		return 1;
	return (int32_t)(g_svga.fifo[SVGA_FIFO_FENCE] - fence) >= 0;
}

// Legacy full drain: SYNC=1 then poll BUSY until the device is idle.
static int svga_legacy_sync(void)
{
	uint64_t start = timer_get_precise_us();

	svga_write_reg(SVGA_REG_SYNC, 1);
	while (svga_read_reg(SVGA_REG_BUSY) != 0) {
		if (timer_get_precise_us() - start > SVGA_BUSY_TIMEOUT_US) {
			WARN_RATELIMIT(1, "svga2: SYNC/BUSY drain timed out");
			return -1;
		}
		if (sched_current())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}
	return 0;
}

int vmsvga2_fence_wait(uint32_t fence, uint64_t timeout_us)
{
	uint64_t start;

	if (!g_svga.present)
		return 0;
	if (fence == 0 || !svga_has_fence())
		return svga_legacy_sync(); // SYNC fallback: global drain

	if (vmsvga2_fence_passed(fence))
		return 0;

	if (g_svga.irq_enabled && svga_has_fifo_reg(SVGA_FIFO_FENCE_GOAL)) {
		// IRQ path: arm the fence goal and park until the interrupt
		// handler wakes us (or timeout).
		task_t *cur = sched_current();

		g_svga.fifo[SVGA_FIFO_FENCE_GOAL] = fence;
		svga_write_reg(SVGA_REG_IRQMASK,
			       SVGA_IRQ_BASE_MASK | SVGA_IRQFLAG_ANY_FENCE |
				       SVGA_IRQFLAG_FENCE_GOAL);
		svga_doorbell();
		start = timer_get_precise_us();
		while (!vmsvga2_fence_passed(fence)) {
			if (timer_get_precise_us() - start > timeout_us) {
				WARN_RATELIMIT(1,
					       "svga2: fence %u wait timeout",
					       fence);
				return -1;
			}
			if (!cur) {
				__asm__ volatile("pause");
				continue;
			}
			uint64_t f;
			cur->wait_channel = (void *)&svga_fence_waiters;
			cur->state = TASK_BLOCKED;
			// Close the lost-wakeup window: re-check under the
			// IRQ lock after publishing BLOCKED (the IRQ handler
			// wakes the channel under this lock).
			spin_lock_irqsave(&svga_irq_lock, &f);
			if (vmsvga2_fence_passed(fence)) {
				cur->state = TASK_READY;
				cur->wait_channel = NULL;
				spin_unlock_irqrestore(&svga_irq_lock, f);
				break;
			}
			cur->wakeup_tick = timer_ticks() + 2; // safety net
			spin_unlock_irqrestore(&svga_irq_lock, f);
			sched_schedule();
			if (signal_pending(cur))
				return -1;
		}
		return 0;
	}

	// Poll path (QEMU has no IRQ support): yield-poll with timeout.
	start = timer_get_precise_us();
	svga_doorbell();
	while (!vmsvga2_fence_passed(fence)) {
		// Reading BUSY nudges on-demand hosts to drain the FIFO.
		(void)svga_read_reg(SVGA_REG_BUSY);
		if (timer_get_precise_us() - start > timeout_us) {
			WARN_RATELIMIT(1, "svga2: fence %u poll timeout",
				       fence);
			return -1;
		}
		if (sched_current())
			sched_yield_in_kernel();
		else
			__asm__ volatile("pause");
	}
	return 0;
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
}

static void vmsvga2_hw_irq_notify(uint32_t status);

// ===========================================================================
// IRQ handling
// ===========================================================================

int vmsvga2_irq(void)
{
	uint32_t status;
	uint64_t f;

	if (!g_vmsvga_initialized)
		return 0;

	// Read+ack: the IRQ status port is write-1-to-clear.
	status = inl(g_svga.io_base + SVGA_IRQSTATUS_PORT);
	if (!status)
		return 0; // shared line, not ours
	outl(g_svga.io_base + SVGA_IRQSTATUS_PORT, status);

	spin_lock_irqsave(&svga_irq_lock, &f);
	g_svga.irq_pending |= status;
	spin_unlock_irqrestore(&svga_irq_lock, f);

	if (status & (SVGA_IRQFLAG_ANY_FENCE | SVGA_IRQFLAG_FENCE_GOAL |
		      SVGA_IRQFLAG_FIFO_PROGRESS))
		sched_wake_channel((void *)&svga_fence_waiters);
	vmsvga2_hw_irq_notify(status);
	return 1;
}

// Route the device's legacy INTx line through the IOAPIC.  Only used when
// the device exposes SVGA_CAP_IRQMASK (VMware/VirtualBox; QEMU does not).
static void svga_irq_init(void)
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
	{
		uint32_t cmd = pci_cfg_read32(dev->bus, dev->device,
					      dev->function, 0x04);
		if (cmd & PCI_CMD_INTX_DISABLE)
			pci_cfg_write32(dev->bus, dev->device, dev->function,
					0x04, cmd & ~PCI_CMD_INTX_DISABLE);
	}

	/* Clear any stale status, then arm the sources that must always be
	 * on.  The fence-goal sources below are armed per wait; these are not
	 * optional -- they are how a command-buffer completion is announced. */
	outl(g_svga.io_base + SVGA_IRQSTATUS_PORT, 0xFF);
	svga_write_reg(SVGA_REG_IRQMASK, SVGA_IRQ_BASE_MASK);

	g_vmsvga_legacy_irq = irq;
	ioapic_configure_legacy_irq(irq, (uint8_t)(32 + irq),
				    IOAPIC_POLARITY_LOW, IOAPIC_TRIGGER_LEVEL);
	g_svga.irq_enabled = 1;
	kprintf("svga2: fence IRQ on GSI %u\n", irq);
}

// ===========================================================================
// Updates
// ===========================================================================

void vmsvga2_update_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	uint32_t cmd[5];

	if (!g_svga.active || w == 0 || h == 0)
		return;
	// Clip to the current mode; callers pass framebuffer coordinates.
	// Print-free (runs as the console flush hook): out-of-bounds rects
	// are silently dropped.
	if (x >= g_svga.crtc.width || y >= g_svga.crtc.height)
		return;
	if (x + w > g_svga.crtc.width)
		w = g_svga.crtc.width - x;
	if (y + h > g_svga.crtc.height)
		h = g_svga.crtc.height - y;

	cmd[0] = SVGA_CMD_UPDATE;
	cmd[1] = x;
	cmd[2] = y;
	cmd[3] = w;
	cmd[4] = h;
	(void)svga_fifo_write_cmd(cmd, sizeof(cmd));
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

// Program a mode; caller holds svga_modeset_mutex.
static int svga_program_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
	uint32_t rb_bpp;

	svga_write_reg(SVGA_REG_ENABLE, 0);
	svga_write_reg(SVGA_REG_WIDTH, width);
	svga_write_reg(SVGA_REG_HEIGHT, height);
	svga_write_reg(SVGA_REG_BITS_PER_PIXEL, bpp);
	svga_write_reg(SVGA_REG_ENABLE, 1);

	// Read back what the host actually accepted.
	g_svga.crtc.width = svga_read_reg(SVGA_REG_WIDTH);
	g_svga.crtc.height = svga_read_reg(SVGA_REG_HEIGHT);
	rb_bpp = svga_read_reg(SVGA_REG_BITS_PER_PIXEL);
	g_svga.crtc.pitch = svga_read_reg(SVGA_REG_BYTES_PER_LINE);
	g_svga.crtc.fb_offset = svga_read_reg(SVGA_REG_FB_OFFSET);
	g_svga.fb_size = svga_read_reg(SVGA_REG_FB_SIZE);
	g_svga.crtc.enabled = 1;

	if (WARN(g_svga.crtc.width != width || g_svga.crtc.height != height,
		 "svga2: host adjusted mode %ux%u -> %ux%u", width, height,
		 g_svga.crtc.width, g_svga.crtc.height)) {
		// Keep going with the host-adjusted geometry.
	}
	WARN_ON_ONCE(rb_bpp != bpp); // host refused the pixel format
	g_svga.crtc.bpp = rb_bpp;

	// Pitch handling: the line stride may exceed width*bytes-per-pixel.
	if (WARN_ON_ONCE(g_svga.crtc.pitch <
			 g_svga.crtc.width * (g_svga.crtc.bpp / 8)))
		return -1;
	// DMA/scanout bound check against VRAM.
	if (WARN(g_svga.crtc.fb_offset +
			 (uint64_t)g_svga.crtc.pitch * g_svga.crtc.height >
		 g_svga.vram_size,
		 "svga2: mode exceeds VRAM"))
		return -1;
	return 0;
}

int vmsvga2_set_mode(uint32_t width, uint32_t height, uint32_t bpp)
{
	int rc;

	if (!g_svga.present)
		return -1;
	if (width == 0 || height == 0)
		return -1;
	if (width > g_svga.max_width || height > g_svga.max_height)
		return -1;
	if (!svga_bpp_supported(bpp))
		return -1;
	// Memory limits: the whole mode must fit the framebuffer BAR.
	if ((uint64_t)width * height * (bpp / 8) > g_svga.vram_size)
		return -1;

	svga_mutex_lock(&svga_modeset_mutex);
	rc = svga_program_mode(width, height, bpp);
	if (rc == 0)
		g_svga.active = 1;
	svga_mutex_unlock(&svga_modeset_mutex);
	svga_report_errors(); // safe context: surface deferred FIFO errors
	return rc;
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
	svga_write_reg(SVGA_REG_TRACES, enable ? 1 : 0);
}

int vmsvga2_display_enable(int enable)
{
	if (!g_svga.present)
		return -1;
	svga_mutex_lock(&svga_modeset_mutex);
	svga_write_reg(SVGA_REG_ENABLE, enable ? 1 : 0);
	g_svga.crtc.enabled = !!enable;
	svga_mutex_unlock(&svga_modeset_mutex);
	if (enable)
		vmsvga2_update_full();
	return 0;
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
// Hardware cursor (cursor_lock protects the cached image, the FIFO bypass
// registers and the scratch command buffer)
// ===========================================================================

#define SVGA_CURSOR_MAX_DIM 64
#define SVGA_CURSOR_ID 0

static struct {
	int valid;
	int alpha; // cached image is ARGB (vs 1-bit AND/XOR)
	uint32_t width, height, hot_x, hot_y;
	uint32_t argb[SVGA_CURSOR_MAX_DIM * SVGA_CURSOR_MAX_DIM];
	uint32_t and_mask[SVGA_CURSOR_MAX_DIM * 2]; // 1bpp rows, dword-padded
	uint32_t xor_mask[SVGA_CURSOR_MAX_DIM * 2];
	// Scratch for building DEFINE_CURSOR commands (max: alpha 64x64)
	uint32_t scratch[8 + SVGA_CURSOR_MAX_DIM * SVGA_CURSOR_MAX_DIM];
} g_cursor;

int vmsvga2_has_hw_cursor(void)
{
	if (!g_svga.present)
		return 0;
	if (!(g_svga.caps & (SVGA_CAP_CURSOR | SVGA_CAP_ALPHA_CURSOR)))
		return 0;
	// Position updates need one of the cursor-bypass mechanisms.
	if (svga_has_fifo_cap(SVGA_FIFO_CAP_CURSOR_BYPASS_3))
		return 1;
	return (g_svga.caps &
		(SVGA_CAP_CURSOR_BYPASS | SVGA_CAP_CURSOR_BYPASS_2)) != 0;
}

// Send the cached cursor image to the host; caller holds svga_cursor_lock.
static int svga_cursor_send_define(void)
{
	uint32_t *cmd = g_cursor.scratch;
	uint32_t words;

	if (!g_cursor.valid)
		return -1;

	if (g_cursor.alpha) {
		cmd[0] = SVGA_CMD_DEFINE_ALPHA_CURSOR;
		cmd[1] = SVGA_CURSOR_ID;
		cmd[2] = g_cursor.hot_x;
		cmd[3] = g_cursor.hot_y;
		cmd[4] = g_cursor.width;
		cmd[5] = g_cursor.height;
		words = g_cursor.width * g_cursor.height;
		kmemcpy(&cmd[6], g_cursor.argb, words * sizeof(uint32_t));
		words += 6;
	} else {
		uint32_t row_dwords = (g_cursor.width + 31) / 32;
		uint32_t mask_dwords = row_dwords * g_cursor.height;

		cmd[0] = SVGA_CMD_DEFINE_CURSOR;
		cmd[1] = SVGA_CURSOR_ID;
		cmd[2] = g_cursor.hot_x;
		cmd[3] = g_cursor.hot_y;
		cmd[4] = g_cursor.width;
		cmd[5] = g_cursor.height;
		cmd[6] = 1; // AND mask depth
		cmd[7] = 1; // XOR mask depth
		kmemcpy(&cmd[8], g_cursor.and_mask,
			mask_dwords * sizeof(uint32_t));
		kmemcpy(&cmd[8 + mask_dwords], g_cursor.xor_mask,
			mask_dwords * sizeof(uint32_t));
		words = 8 + 2 * mask_dwords;
	}
	return svga_fifo_write_cmd(cmd, words * sizeof(uint32_t));
}

int vmsvga2_cursor_define(uint32_t width, uint32_t height, uint32_t hot_x,
			  uint32_t hot_y, const uint32_t *and_mask,
			  const uint32_t *xor_mask)
{
	uint64_t f;
	uint32_t mask_dwords;
	int rc;

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
	rc = svga_cursor_send_define();
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return rc;
}

int vmsvga2_cursor_define_alpha(uint32_t width, uint32_t height,
				uint32_t hot_x, uint32_t hot_y,
				const uint32_t *argb_pixels)
{
	uint64_t f;
	int rc;

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
	rc = svga_cursor_send_define();
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return rc;
}

int vmsvga2_cursor_move(int32_t x, int32_t y, int visible)
{
	uint64_t f;

	if (!g_svga.active)
		return -1;
	if (!vmsvga2_has_hw_cursor())
		return -1;

	spin_lock_irqsave(&svga_cursor_lock, &f);
	if (svga_has_fifo_cap(SVGA_FIFO_CAP_CURSOR_BYPASS_3)) {
		// Lowest-latency path: direct FIFO registers, no command.
		g_svga.fifo[SVGA_FIFO_CURSOR_X] = (uint32_t)x;
		g_svga.fifo[SVGA_FIFO_CURSOR_Y] = (uint32_t)y;
		g_svga.fifo[SVGA_FIFO_CURSOR_ON] = visible ? 1 : 0;
		if (svga_has_fifo_reg(SVGA_FIFO_CURSOR_SCREEN_ID))
			g_svga.fifo[SVGA_FIFO_CURSOR_SCREEN_ID] =
				SVGA_ID_INVALID_SCREEN;
		__asm__ volatile("" ::: "memory");
		g_svga.fifo[SVGA_FIFO_CURSOR_COUNT]++;
	} else {
		svga_write_reg(SVGA_REG_CURSOR_ID, SVGA_CURSOR_ID);
		svga_write_reg(SVGA_REG_CURSOR_X, (uint32_t)x);
		svga_write_reg(SVGA_REG_CURSOR_Y, (uint32_t)y);
		svga_write_reg(SVGA_REG_CURSOR_ON, visible ? 1 : 0);
	}
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return 0;
}

int vmsvga2_cursor_show(int visible)
{
	uint64_t f;

	if (!g_svga.active || !vmsvga2_has_hw_cursor())
		return -1;
	spin_lock_irqsave(&svga_cursor_lock, &f);
	if (svga_has_fifo_cap(SVGA_FIFO_CAP_CURSOR_BYPASS_3)) {
		g_svga.fifo[SVGA_FIFO_CURSOR_ON] = visible ? 1 : 0;
		__asm__ volatile("" ::: "memory");
		g_svga.fifo[SVGA_FIFO_CURSOR_COUNT]++;
	} else {
		svga_write_reg(SVGA_REG_CURSOR_ON, visible ? 1 : 0);
	}
	/* Hiding also goes through the legacy register, whichever path was
	 * used above.
	 *
	 * The two are meant to be the same control, but they are implemented
	 * separately, and a host that honours only one of them leaves the
	 * cursor on screen after the other has been told to take it away.
	 * Getting that wrong is visible -- a second pointer beside the one the
	 * program that owns the display is drawing -- so the hide is asserted
	 * both ways.  Showing is not: exactly one path should own the position
	 * updates, and that is decided above. */
	if (!visible)
		svga_write_reg(SVGA_REG_CURSOR_ON, 0);
	spin_unlock_irqrestore(&svga_cursor_lock, f);
	return 0;
}

// ===========================================================================
// Error recovery / device reset / suspend-resume
// ===========================================================================

static int svga_negotiate_version(void);

// Full reinitialization after FIFO corruption, VM reset or resume.  Re-runs
// version negotiation, FIFO setup, the current mode and the cursor image.
int vmsvga2_reset(void)
{
	uint32_t w, h, bpp;
	uint64_t f;
	int rc = 0;

	if (!g_svga.present)
		return -1;

	svga_mutex_lock(&svga_modeset_mutex);
	w = g_svga.crtc.width;
	h = g_svga.crtc.height;
	bpp = g_svga.crtc.bpp;

	svga_write_reg(SVGA_REG_ENABLE, 0);
	if (svga_negotiate_version() != 0) {
		svga_mutex_unlock(&svga_modeset_mutex);
		WARN(1, "svga2: reset: device vanished");
		return -1;
	}

	// Reset FIFO cursors and reconfigure (the mapping is unchanged).
	spin_lock_irqsave(&svga_fifo_lock, &f);
	{
		uint32_t min;
		if (g_svga.caps & SVGA_CAP_EXTENDED_FIFO)
			min = SVGA_FIFO_NUM_REGS * sizeof(uint32_t);
		else
			min = 4 * sizeof(uint32_t);
		g_svga.fifo[SVGA_FIFO_MIN] = min;
		g_svga.fifo[SVGA_FIFO_MAX] = g_svga.fifo_size;
		g_svga.fifo[SVGA_FIFO_NEXT_CMD] = min;
		g_svga.fifo[SVGA_FIFO_STOP] = min;
		g_svga.reserved_bytes = 0;
		g_svga.reserved_in_place = 0;
	}
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	svga_write_reg(SVGA_REG_CONFIG_DONE, 1);
	svga_write_reg(SVGA_REG_GUEST_ID, SVGA_GUEST_ID_OTHER);
	if (g_svga.irq_enabled) {
		/* Re-arm rather than silence: a mode set happens whenever the
		 * screen is resized, and leaving the mask at zero afterwards
		 * turned the fence interrupt off for the rest of the session. */
		outl(g_svga.io_base + SVGA_IRQSTATUS_PORT, 0xFF);
		svga_write_reg(SVGA_REG_IRQMASK, SVGA_IRQ_BASE_MASK);
	}

	if (g_svga.active && w && h)
		rc = svga_program_mode(w, h, bpp);
	svga_mutex_unlock(&svga_modeset_mutex);

	// Restore the cursor image if one was defined.
	spin_lock_irqsave(&svga_cursor_lock, &f);
	if (g_cursor.valid)
		(void)svga_cursor_send_define();
	spin_unlock_irqrestore(&svga_cursor_lock, f);

	if (rc == 0 && g_svga.active)
		vmsvga2_update_full();
	svga_report_errors();
	return rc;
}

int vmsvga2_suspend(void)
{
	if (!g_svga.present)
		return -1;
	// Drain everything so the host holds no partially-written commands
	// across a VM suspend/snapshot.
	vmsvga2_fifo_flush();
	return 0;
}

int vmsvga2_resume(void)
{
	// After VM resume the device may have lost register and FIFO state;
	// a full reset rebuilds it.
	return vmsvga2_reset();
}

// ===========================================================================
// Capability / limit reporting
// ===========================================================================

int vmsvga2_active(void)
{
	return g_svga.active;
}

uint32_t vmsvga2_get_caps(void)
{
	return g_svga.caps;
}

uint32_t vmsvga2_get_fifo_caps(void)
{
	return g_svga.fifo_caps;
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

svga_pixel_format_t vmsvga2_get_pixel_format(void)
{
	if (!g_svga.active)
		return SVGA_PIXFMT_INVALID;
	if (g_svga.crtc.bpp == 32)
		return SVGA_PIXFMT_XRGB8888;
	if (g_svga.crtc.bpp == 16)
		return SVGA_PIXFMT_RGB565;
	return SVGA_PIXFMT_INVALID;
}

// ===========================================================================
// Guest memory regions (GMR): DMA-visible guest pages the host can read and
// write directly.  Two mechanisms, both capability-gated: GMR2 (FIFO
// commands, page-list based) and legacy GMR (register-programmed descriptor
// chains).  All page addresses are page-aligned by construction (DMA
// alignment requirement of the device).
// ===========================================================================

static spinlock_t svga_gmr_lock = SPINLOCK_INIT("svga_gmr");

typedef struct {
	int used;
	uint32_t num_pages;
	uint64_t desc_phys; // legacy descriptor page (0 when GMR2)
} svga_gmr_t;

static svga_gmr_t g_gmrs[SVGA_MAX_GMRS];

static int svga_has_gmr2(void)
{
	return (g_svga.caps & SVGA_CAP_GMR2) != 0;
}

static int svga_has_gmr(void)
{
	return (g_svga.caps & (SVGA_CAP_GMR | SVGA_CAP_GMR2)) != 0;
}

int vmsvga2_gmr_alloc(uint32_t num_pages)
{
	uint64_t f;
	uint32_t max_ids;
	int id;

	if (!g_svga.present || !svga_has_gmr() || num_pages == 0)
		return -1;
	max_ids = svga_read_reg(SVGA_REG_GMR_MAX_IDS);
	if (max_ids > SVGA_MAX_GMRS)
		max_ids = SVGA_MAX_GMRS;
	if (svga_has_gmr2()) {
		uint32_t max_pages = svga_read_reg(SVGA_REG_GMRS_MAX_PAGES);
		if (max_pages && num_pages > max_pages)
			return -1;
	}

	spin_lock_irqsave(&svga_gmr_lock, &f);
	for (id = 0; id < (int)max_ids; id++) {
		if (!g_gmrs[id].used) {
			g_gmrs[id].used = 1;
			g_gmrs[id].num_pages = num_pages;
			g_gmrs[id].desc_phys = 0;
			spin_unlock_irqrestore(&svga_gmr_lock, f);
			return id;
		}
	}
	spin_unlock_irqrestore(&svga_gmr_lock, f);
	return -1;
}

// Bind guest pages to a GMR id.  page_phys[] entries must be page-aligned.
int vmsvga2_gmr_bind(int gmr_id, const uint64_t *page_phys,
		     uint32_t num_pages)
{
	uint32_t i;

	if (!g_svga.present || gmr_id < 0 || gmr_id >= SVGA_MAX_GMRS)
		return -1;
	if (!g_gmrs[gmr_id].used || !page_phys || num_pages == 0)
		return -1;
	if (WARN_ON_ONCE(num_pages != g_gmrs[gmr_id].num_pages))
		return -1;
	for (i = 0; i < num_pages; i++)
		if (WARN_ON_ONCE(page_phys[i] & (PAGE_SIZE - 1)))
			return -1; // DMA alignment violation

	if (svga_has_gmr2()) {
		// DEFINE_GMR2 + REMAP_GMR2 with a 64-bit PPN list.
		uint32_t hdr_words = 1 + 2; // cmd + {gmrId, numPages}
		uint32_t remap_words = 1 + 4; // cmd + {gmrId,flags,off,num}
		uint32_t total_bytes = (hdr_words + remap_words) * 4 +
				       num_pages * (uint32_t)sizeof(uint64_t);
		uint64_t f;
		uint32_t *cmd;

		// One reservation for both commands + the PPN list so the
		// sequence is atomic in the FIFO.
		if (total_bytes > g_svga.bounce_size &&
		    total_bytes > g_svga.fifo_size / 2)
			return -1; // page list too large for one command

		spin_lock_irqsave(&svga_fifo_lock, &f);
		cmd = svga_fifo_reserve(total_bytes);
		if (!cmd) {
			spin_unlock_irqrestore(&svga_fifo_lock, f);
			return -1;
		}
		cmd[0] = SVGA_CMD_DEFINE_GMR2;
		cmd[1] = (uint32_t)gmr_id;
		cmd[2] = num_pages;
		cmd[3] = SVGA_CMD_REMAP_GMR2;
		cmd[4] = (uint32_t)gmr_id;
		cmd[5] = SVGA_REMAP_GMR2_PPN64;
		cmd[6] = 0; // offsetPages
		cmd[7] = num_pages;
		{
			uint64_t *ppns = (uint64_t *)&cmd[8];
			for (i = 0; i < num_pages; i++)
				ppns[i] = page_phys[i] >> 12;
		}
		svga_fifo_commit(total_bytes);
		spin_unlock_irqrestore(&svga_fifo_lock, f);
		svga_doorbell();
		return 0;
	}

	// Legacy GMR: build a descriptor page (ppn/num_pages run list,
	// {0,0}-terminated) and hand its PPN to the device.  One 4K page
	// holds 511 descriptors + terminator; runs of contiguous pages are
	// coalesced, so this covers any realistic buffer object.
	{
		uint64_t desc_phys = mm_allocate_physical_page();
		svga_guest_mem_descriptor_t *desc;
		uint32_t d = 0;

		if (!desc_phys)
			return -1;
		desc = (svga_guest_mem_descriptor_t *)phys_to_virt(desc_phys);
		for (i = 0; i < num_pages && d < 510; i++) {
			uint32_t ppn = (uint32_t)(page_phys[i] >> 12);

			// Coalesce runs of physically contiguous pages.
			if (d > 0 &&
			    ppn == desc[d - 1].ppn + desc[d - 1].num_pages) {
				desc[d - 1].num_pages++;
				continue;
			}
			desc[d].ppn = ppn;
			desc[d].num_pages = 1;
			d++;
		}
		if (i < num_pages) {
			// Page list too fragmented for a single descriptor
			// page; chaining is not implemented (buffer objects
			// use contiguous memory, which always coalesces).
			mm_free_physical_page(desc_phys);
			WARN_ON_ONCE(1);
			return -1;
		}
		desc[d].ppn = 0;
		desc[d].num_pages = 0; // terminator

		svga_write_reg(SVGA_REG_GMR_ID, (uint32_t)gmr_id);
		svga_write_reg(SVGA_REG_GMR_DESCRIPTOR,
			       (uint32_t)(desc_phys >> 12));
		g_gmrs[gmr_id].desc_phys = desc_phys;
		return 0;
	}
}

int vmsvga2_gmr_free(int gmr_id)
{
	uint64_t f;

	if (!g_svga.present || gmr_id < 0 || gmr_id >= SVGA_MAX_GMRS)
		return -1;
	spin_lock_irqsave(&svga_gmr_lock, &f);
	if (!g_gmrs[gmr_id].used) {
		spin_unlock_irqrestore(&svga_gmr_lock, f);
		return -1;
	}
	g_gmrs[gmr_id].used = 0;
	spin_unlock_irqrestore(&svga_gmr_lock, f);

	// Ensure the host is done with the region before unbinding.
	vmsvga2_fifo_flush();
	if (svga_has_gmr2()) {
		uint32_t cmd[3] = { SVGA_CMD_DEFINE_GMR2, (uint32_t)gmr_id,
				    0 };
		(void)svga_fifo_write_cmd(cmd, sizeof(cmd));
	} else {
		svga_write_reg(SVGA_REG_GMR_ID, (uint32_t)gmr_id);
		svga_write_reg(SVGA_REG_GMR_DESCRIPTOR, 0);
	}
	if (g_gmrs[gmr_id].desc_phys) {
		mm_free_physical_page(g_gmrs[gmr_id].desc_phys);
		g_gmrs[gmr_id].desc_phys = 0;
	}
	return 0;
}

// ===========================================================================
// Buffer objects: GEM/TTM-like refcounted memory objects, GMR-backed when
// the host supports it.  The foundation for a future full display-manager
// (DRM-style) buffer sharing model.
// ===========================================================================

struct svga_bo {
	refcount_t ref;
	spinlock_t lock; // guards nothing mutable yet; per-object lock for
			 // future CPU/device access arbitration
	uint64_t size;
	uint64_t phys; // physically contiguous backing store
	uint32_t pages;
	void *kvirt;
	int gmr_id; // bound GMR id, -1 when host has no GMR support
};

svga_bo_t *svga_bo_create(uint64_t size)
{
	svga_bo_t *bo;
	uint32_t pages;

	if (!g_svga.present || size == 0)
		return NULL;
	pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);

	bo = kalloc(sizeof(*bo));
	if (!bo)
		return NULL;
	kmemset(bo, 0, sizeof(*bo));
	refcount_set(&bo->ref, 1);
	spinlock_init(&bo->lock, "svga_bo");
	bo->size = size;
	bo->pages = pages;
	bo->gmr_id = -1;

	bo->phys = mm_allocate_contiguous_pages(pages);
	if (!bo->phys) {
		kfree(bo);
		return NULL;
	}
	bo->kvirt = (void *)phys_to_virt(bo->phys);

	// Bind as a GMR when the host supports guest memory regions; a BO
	// without a GMR is still usable as a CPU-side staging buffer.
	if (svga_has_gmr()) {
		int id = vmsvga2_gmr_alloc(pages);
		if (id >= 0) {
			// Contiguous allocation: build the page list on the
			// fly (pages are consecutive).
			uint64_t *list = kalloc(pages * sizeof(uint64_t));
			if (list) {
				uint32_t i;
				for (i = 0; i < pages; i++)
					list[i] = bo->phys +
						  (uint64_t)i * PAGE_SIZE;
				if (vmsvga2_gmr_bind(id, list, pages) == 0)
					bo->gmr_id = id;
				else
					vmsvga2_gmr_free(id);
				kfree(list);
			} else {
				vmsvga2_gmr_free(id);
			}
		}
	}
	return bo;
}

void svga_bo_ref(svga_bo_t *bo)
{
	if (!bo)
		return;
	refcount_inc(&bo->ref);
}

void svga_bo_unref(svga_bo_t *bo)
{
	if (!bo)
		return;
	if (!refcount_dec_and_test(&bo->ref))
		return;
	if (bo->gmr_id >= 0)
		vmsvga2_gmr_free(bo->gmr_id);
	if (bo->phys)
		mm_free_contiguous_pages(bo->phys, bo->pages);
	kfree(bo);
}

void *svga_bo_map(svga_bo_t *bo)
{
	return bo ? bo->kvirt : NULL;
}

uint64_t svga_bo_size(svga_bo_t *bo)
{
	return bo ? bo->size : 0;
}

int svga_bo_gmr_id(svga_bo_t *bo)
{
	return bo ? bo->gmr_id : -1;
}

// ===========================================================================
// Screen objects: independent display surfaces (multi-monitor foundation).
// Presentation binds guest memory (GMRFB) to a screen via blits; the driver
// tracks defined screens for host-visibility bookkeeping.
// ===========================================================================

static spinlock_t svga_screen_lock = SPINLOCK_INIT("svga_screen");

typedef struct {
	int used;
	int32_t x, y;
	uint32_t w, h;
	uint32_t flags;
} svga_screen_t;

static svga_screen_t g_screens[SVGA_MAX_SCREENS];

static int svga_has_screen_object(void)
{
	return svga_has_fifo_cap(SVGA_FIFO_CAP_SCREEN_OBJECT |
				 SVGA_FIFO_CAP_SCREEN_OBJECT_2);
}

int vmsvga2_screen_define(uint32_t id, int32_t x, int32_t y, uint32_t w,
			  uint32_t h, uint32_t flags)
{
	uint32_t cmd[1 + 11];
	uint32_t words;
	uint64_t f;
	int so2;
	int rc;

	if (!g_svga.present || !svga_has_screen_object())
		return -1;
	if (id >= SVGA_MAX_SCREENS || w == 0 || h == 0)
		return -1;

	// Screen object v1: {structSize,id,flags,size,root} (7 dwords).
	// v2 appends {backingStore{gmrId,offset,pitch}, cloneCount}.
	so2 = svga_has_fifo_cap(SVGA_FIFO_CAP_SCREEN_OBJECT_2);
	words = so2 ? 12 : 8; // command dword + struct
	cmd[0] = SVGA_CMD_DEFINE_SCREEN;
	cmd[1] = (words - 1) * sizeof(uint32_t); // structSize
	cmd[2] = id;
	cmd[3] = flags | SVGA_SCREEN_MUST_BE_SET;
	cmd[4] = w;
	cmd[5] = h;
	cmd[6] = (uint32_t)x;
	cmd[7] = (uint32_t)y;
	if (so2) {
		cmd[8] = SVGA_GMR_NULL; // backingStore.ptr.gmrId (none)
		cmd[9] = 0; // backingStore.ptr.offset
		cmd[10] = 0; // backingStore.pitch
		cmd[11] = 0; // cloneCount
	}
	rc = svga_fifo_write_cmd(cmd, words * sizeof(uint32_t));
	if (rc == 0) {
		spin_lock_irqsave(&svga_screen_lock, &f);
		g_screens[id].used = 1;
		g_screens[id].x = x;
		g_screens[id].y = y;
		g_screens[id].w = w;
		g_screens[id].h = h;
		g_screens[id].flags = flags;
		spin_unlock_irqrestore(&svga_screen_lock, f);
	}
	return rc;
}

int vmsvga2_screen_destroy(uint32_t id)
{
	uint32_t cmd[2];
	uint64_t f;

	if (!g_svga.present || !svga_has_screen_object())
		return -1;
	if (id >= SVGA_MAX_SCREENS || !g_screens[id].used)
		return -1;
	cmd[0] = SVGA_CMD_DESTROY_SCREEN;
	cmd[1] = id;
	if (svga_fifo_write_cmd(cmd, sizeof(cmd)) != 0)
		return -1;
	spin_lock_irqsave(&svga_screen_lock, &f);
	g_screens[id].used = 0;
	spin_unlock_irqrestore(&svga_screen_lock, f);
	return 0;
}

int vmsvga2_num_screens(void)
{
	uint64_t f;
	int i, n = 0;

	spin_lock_irqsave(&svga_screen_lock, &f);
	for (i = 0; i < SVGA_MAX_SCREENS; i++)
		if (g_screens[i].used)
			n++;
	spin_unlock_irqrestore(&svga_screen_lock, f);
	return n;
}

// Present a buffer object onto a screen object (GMRFB blit path).
int vmsvga2_screen_present(uint32_t screen_id, svga_bo_t *bo, uint32_t bo_pitch,
			   int32_t dst_x, int32_t dst_y, uint32_t w,
			   uint32_t h)
{
	uint32_t gmrfb[1 + 4];
	uint32_t blit[1 + 7];
	uint64_t f;
	void *dst;

	if (!g_svga.present || !svga_has_screen_object())
		return -1;
	if (screen_id >= SVGA_MAX_SCREENS || !g_screens[screen_id].used)
		return -1;
	if (!bo || bo->gmr_id < 0 || w == 0 || h == 0)
		return -1;

	// SVGA_CMD_DEFINE_GMRFB: {ptr{gmrId,offset}, bytesPerLine,
	// format{bpp,depth}}
	gmrfb[0] = SVGA_CMD_DEFINE_GMRFB;
	gmrfb[1] = (uint32_t)bo->gmr_id;
	gmrfb[2] = 0; // offset
	gmrfb[3] = bo_pitch;
	gmrfb[4] = 32 | (24 << 8); // XRGB8888

	// SVGA_CMD_BLIT_GMRFB_TO_SCREEN: {srcOrigin{x,y},
	// destRect{l,t,r,b}, destScreenId}
	blit[0] = SVGA_CMD_BLIT_GMRFB_TO_SCREEN;
	blit[1] = 0; // src x
	blit[2] = 0; // src y
	blit[3] = (uint32_t)dst_x;
	blit[4] = (uint32_t)dst_y;
	blit[5] = (uint32_t)(dst_x + (int32_t)w);
	blit[6] = (uint32_t)(dst_y + (int32_t)h);
	blit[7] = screen_id;

	// One reservation for both commands (atomic GMRFB+blit pair, so a
	// concurrent present cannot re-point the GMRFB in between).
	spin_lock_irqsave(&svga_fifo_lock, &f);
	dst = svga_fifo_reserve(sizeof(gmrfb) + sizeof(blit));
	if (!dst) {
		spin_unlock_irqrestore(&svga_fifo_lock, f);
		return -1;
	}
	kmemcpy(dst, gmrfb, sizeof(gmrfb));
	kmemcpy((uint8_t *)dst + sizeof(gmrfb), blit, sizeof(blit));
	svga_fifo_commit(sizeof(gmrfb) + sizeof(blit));
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	svga_doorbell();
	return 0;
}

// ===========================================================================
// Surfaces and 3D contexts (SVGA3D subset).  Only available when the host
// exposes the 3D capability (VMware/VirtualBox with 3D acceleration); QEMU
// reports no 3D and all entry points degrade to -1.  Offscreen surfaces are
// simply surfaces that are never presented.
// ===========================================================================

static spinlock_t svga_surface_lock = SPINLOCK_INIT("svga_surface");

typedef struct {
	int used;
	uint32_t width, height, format;
} svga_surface_t;

static svga_surface_t g_surfaces[SVGA_MAX_SURFACES];
static uint8_t g_contexts[SVGA_MAX_CONTEXTS];

static int svga_has_3d(void)
{
	if (!(g_svga.caps & SVGA_CAP_3D))
		return 0;
	if (!svga_has_fifo_reg(SVGA_FIFO_3D_HWVERSION))
		return 0;
	return g_svga.fifo[SVGA_FIFO_3D_HWVERSION] != 0;
}

// Emit one SVGA3D command (header + body dwords); body may be NULL for
// commands built directly into the reservation by the caller.
static int svga_3d_cmd(uint32_t cmd_id, const uint32_t *body,
		       uint32_t body_words)
{
	uint64_t f;
	uint32_t *dst;
	uint32_t bytes = (2 + body_words) * sizeof(uint32_t);

	spin_lock_irqsave(&svga_fifo_lock, &f);
	dst = svga_fifo_reserve(bytes);
	if (!dst) {
		spin_unlock_irqrestore(&svga_fifo_lock, f);
		return -1;
	}
	dst[0] = cmd_id;
	dst[1] = body_words * sizeof(uint32_t);
	if (body_words)
		kmemcpy(&dst[2], body, body_words * sizeof(uint32_t));
	svga_fifo_commit(bytes);
	spin_unlock_irqrestore(&svga_fifo_lock, f);
	svga_doorbell();
	return 0;
}

int vmsvga2_surface_define(uint32_t width, uint32_t height, uint32_t format,
			   uint32_t flags)
{
	// body: sid, surfaceFlags, format, face[6].numMipLevels,
	//       mipSize {w,h,d}
	uint32_t body[3 + 6 + 3];
	uint64_t f;
	int sid;

	if (!g_svga.present || !svga_has_3d())
		return -1;
	if (width == 0 || height == 0)
		return -1;

	spin_lock_irqsave(&svga_surface_lock, &f);
	for (sid = 0; sid < SVGA_MAX_SURFACES; sid++)
		if (!g_surfaces[sid].used)
			break;
	if (sid == SVGA_MAX_SURFACES) {
		spin_unlock_irqrestore(&svga_surface_lock, f);
		return -1;
	}
	g_surfaces[sid].used = 1;
	g_surfaces[sid].width = width;
	g_surfaces[sid].height = height;
	g_surfaces[sid].format = format;
	spin_unlock_irqrestore(&svga_surface_lock, f);

	body[0] = (uint32_t)sid;
	body[1] = flags;
	body[2] = format;
	body[3] = 1; // face 0: one mip level
	body[4] = body[5] = body[6] = body[7] = body[8] = 0;
	body[9] = width;
	body[10] = height;
	body[11] = 1; // depth
	if (svga_3d_cmd(SVGA_3D_CMD_SURFACE_DEFINE, body,
			sizeof(body) / sizeof(body[0])) != 0) {
		g_surfaces[sid].used = 0;
		return -1;
	}
	return sid;
}

int vmsvga2_surface_destroy(int sid)
{
	uint32_t body[1];
	uint64_t f;

	if (!g_svga.present || !svga_has_3d())
		return -1;
	if (sid < 0 || sid >= SVGA_MAX_SURFACES || !g_surfaces[sid].used)
		return -1;
	body[0] = (uint32_t)sid;
	if (svga_3d_cmd(SVGA_3D_CMD_SURFACE_DESTROY, body, 1) != 0)
		return -1;
	spin_lock_irqsave(&svga_surface_lock, &f);
	g_surfaces[sid].used = 0;
	spin_unlock_irqrestore(&svga_surface_lock, f);
	return 0;
}

// Shared implementation of surface DMA in both directions.
static int svga_surface_dma(int sid, svga_bo_t *bo, uint32_t x, uint32_t y,
			    uint32_t w, uint32_t h, uint32_t pitch,
			    uint32_t transfer)
{
	// body: guestImage{ptr{gmrId,offset},pitch}, hostImage{sid,face,mip},
	//       transfer, copyBox{x,y,z,w,h,d,srcx,srcy,srcz},
	//       suffix{suffixSize,maximumOffset,flags}
	uint32_t body[3 + 3 + 1 + 9 + 3];

	if (!g_svga.present || !svga_has_3d())
		return -1;
	if (sid < 0 || sid >= SVGA_MAX_SURFACES || !g_surfaces[sid].used)
		return -1;
	if (!bo || bo->gmr_id < 0 || w == 0 || h == 0 || pitch == 0)
		return -1;
	// DMA bound check against the buffer object.
	if (WARN_ON_ONCE((uint64_t)pitch * h > bo->size))
		return -1;

	body[0] = (uint32_t)bo->gmr_id;
	body[1] = 0; // offset
	body[2] = pitch;
	body[3] = (uint32_t)sid;
	body[4] = 0; // face
	body[5] = 0; // mipmap
	body[6] = transfer;
	body[7] = x;
	body[8] = y;
	body[9] = 0; // z
	body[10] = w;
	body[11] = h;
	body[12] = 1; // depth
	body[13] = 0; // srcx
	body[14] = 0; // srcy
	body[15] = 0; // srcz
	body[16] = 3 * sizeof(uint32_t); // suffixSize
	body[17] = pitch * h - 1; // maximumOffset
	body[18] = 0; // flags (synchronized)
	return svga_3d_cmd(SVGA_3D_CMD_SURFACE_DMA, body,
			   sizeof(body) / sizeof(body[0]));
}

int vmsvga2_surface_dma_to(int sid, svga_bo_t *bo, uint32_t x, uint32_t y,
			   uint32_t w, uint32_t h, uint32_t pitch)
{
	return svga_surface_dma(sid, bo, x, y, w, h, pitch,
				SVGA3D_WRITE_HOST_VRAM);
}

int vmsvga2_surface_dma_from(int sid, svga_bo_t *bo, uint32_t x, uint32_t y,
			     uint32_t w, uint32_t h, uint32_t pitch)
{
	int rc = svga_surface_dma(sid, bo, x, y, w, h, pitch,
				  SVGA3D_READ_HOST_VRAM);
	if (rc == 0) {
		// Downloads must complete before the CPU reads the buffer.
		uint32_t fence = vmsvga2_fence_insert();
		if (fence)
			rc = vmsvga2_fence_wait(fence, SVGA_FENCE_TIMEOUT_US);
		else
			rc = svga_legacy_sync();
	}
	return rc;
}

int vmsvga2_surface_copy(int src_sid, int dst_sid, uint32_t sx, uint32_t sy,
			 uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
	// body: src{sid,face,mip}, dest{sid,face,mip},
	//       copyBox{x,y,z,w,h,d,srcx,srcy,srcz}
	uint32_t body[3 + 3 + 9];

	if (!g_svga.present || !svga_has_3d())
		return -1;
	if (src_sid < 0 || src_sid >= SVGA_MAX_SURFACES ||
	    !g_surfaces[src_sid].used)
		return -1;
	if (dst_sid < 0 || dst_sid >= SVGA_MAX_SURFACES ||
	    !g_surfaces[dst_sid].used)
		return -1;
	body[0] = (uint32_t)src_sid;
	body[1] = 0;
	body[2] = 0;
	body[3] = (uint32_t)dst_sid;
	body[4] = 0;
	body[5] = 0;
	body[6] = dx;
	body[7] = dy;
	body[8] = 0;
	body[9] = w;
	body[10] = h;
	body[11] = 1;
	body[12] = sx;
	body[13] = sy;
	body[14] = 0;
	return svga_3d_cmd(SVGA_3D_CMD_SURFACE_COPY, body,
			   sizeof(body) / sizeof(body[0]));
}

// Front-buffer presentation of a surface (copy to the visible display).
int vmsvga2_surface_present(int sid, uint32_t x, uint32_t y, uint32_t w,
			    uint32_t h)
{
	// body: sid, copyRect{x,y,w,h,srcx,srcy}
	uint32_t body[1 + 6];

	if (!g_svga.present || !svga_has_3d())
		return -1;
	if (sid < 0 || sid >= SVGA_MAX_SURFACES || !g_surfaces[sid].used)
		return -1;
	body[0] = (uint32_t)sid;
	body[1] = x;
	body[2] = y;
	body[3] = w;
	body[4] = h;
	body[5] = 0; // srcx
	body[6] = 0; // srcy
	return svga_3d_cmd(SVGA_3D_CMD_PRESENT, body,
			   sizeof(body) / sizeof(body[0]));
}

int vmsvga2_context_create(void)
{
	uint64_t f;
	int cid;
	uint32_t body[1];

	if (!g_svga.present || !svga_has_3d())
		return -1;
	spin_lock_irqsave(&svga_surface_lock, &f);
	for (cid = 0; cid < SVGA_MAX_CONTEXTS; cid++)
		if (!g_contexts[cid])
			break;
	if (cid == SVGA_MAX_CONTEXTS) {
		spin_unlock_irqrestore(&svga_surface_lock, f);
		return -1;
	}
	g_contexts[cid] = 1;
	spin_unlock_irqrestore(&svga_surface_lock, f);
	body[0] = (uint32_t)cid;
	if (svga_3d_cmd(SVGA_3D_CMD_CONTEXT_DEFINE, body, 1) != 0) {
		g_contexts[cid] = 0;
		return -1;
	}
	return cid;
}

int vmsvga2_context_destroy(int cid)
{
	uint32_t body[1];

	if (!g_svga.present || !svga_has_3d())
		return -1;
	if (cid < 0 || cid >= SVGA_MAX_CONTEXTS || !g_contexts[cid])
		return -1;
	body[0] = (uint32_t)cid;
	if (svga_3d_cmd(SVGA_3D_CMD_CONTEXT_DESTROY, body, 1) != 0)
		return -1;
	g_contexts[cid] = 0;
	return 0;
}

// ===========================================================================
// Accelerated 2D front-buffer operations (EXA-style hooks)
// ===========================================================================

static int svga_accel_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			   uint32_t color)
{
	uint32_t cmd[7];

	if (!g_svga.active ||
	    !svga_has_fifo_cap(SVGA_FIFO_CAP_ACCELFRONT))
		return -1;
	cmd[0] = SVGA_CMD_FRONT_ROP_FILL;
	cmd[1] = color;
	cmd[2] = x;
	cmd[3] = y;
	cmd[4] = w;
	cmd[5] = h;
	cmd[6] = SVGA_ROP_COPY;
	return svga_fifo_write_cmd(cmd, sizeof(cmd));
}

static int svga_accel_copy(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
			   uint32_t w, uint32_t h)
{
	uint32_t cmd[7];

	if (!g_svga.active || !(g_svga.caps & SVGA_CAP_RECT_COPY))
		return -1;
	cmd[0] = SVGA_CMD_RECT_COPY;
	cmd[1] = sx;
	cmd[2] = sy;
	cmd[3] = dx;
	cmd[4] = dy;
	cmd[5] = w;
	cmd[6] = h;
	return svga_fifo_write_cmd(cmd, sizeof(cmd));
}

static int svga_accel_sync(void)
{
	if (!g_svga.present)
		return -1;
	vmsvga2_fifo_flush();
	return 0;
}

static const svga_accel_ops_t g_accel_ops = {
	.fill = svga_accel_fill,
	.copy = svga_accel_copy,
	.sync = svga_accel_sync,
};

const svga_accel_ops_t *vmsvga2_get_accel_ops(void)
{
	if (!g_svga.present)
		return NULL;
	// The ops are individually capability-checked; expose them whenever
	// at least screen-to-screen copies are available.
	if (!(g_svga.caps & SVGA_CAP_RECT_COPY) &&
	    !svga_has_fifo_cap(SVGA_FIFO_CAP_ACCELFRONT))
		return NULL;
	return &g_accel_ops;
}

// ===========================================================================
// Video overlay (ESCAPE-based register sets)
// ===========================================================================

int vmsvga2_has_overlay(void)
{
	return g_svga.present && svga_has_fifo_cap(SVGA_FIFO_CAP_VIDEO) &&
	       svga_has_fifo_cap(SVGA_FIFO_CAP_ESCAPE);
}

// Set overlay registers for one stream unit.  regs[] is indexed by
// SVGA_VIDEO_* register id; num_regs entries are sent.
int vmsvga2_overlay_set(uint32_t unit, const uint32_t *regs,
			uint32_t num_regs)
{
	uint32_t cmd[3 + 2 + 2 * SVGA_VIDEO_NUM_REGS];
	uint32_t i, items;

	if (!vmsvga2_has_overlay() || !regs)
		return -1;
	if (num_regs > SVGA_VIDEO_NUM_REGS)
		num_regs = SVGA_VIDEO_NUM_REGS;
	items = num_regs;

	// SVGA_CMD_ESCAPE {nsid, size} + {cmdType, streamId} + item pairs
	cmd[0] = SVGA_CMD_ESCAPE;
	cmd[1] = SVGA_ESCAPE_NSID_VMWARE;
	cmd[2] = (2 + 2 * items) * sizeof(uint32_t);
	cmd[3] = SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS;
	cmd[4] = unit;
	for (i = 0; i < items; i++) {
		cmd[5 + 2 * i] = i; // registerId
		cmd[6 + 2 * i] = regs[i];
	}
	return svga_fifo_write_cmd(cmd, (5 + 2 * items) * sizeof(uint32_t));
}

int vmsvga2_overlay_flush(uint32_t unit)
{
	uint32_t cmd[5];

	if (!vmsvga2_has_overlay())
		return -1;
	cmd[0] = SVGA_CMD_ESCAPE;
	cmd[1] = SVGA_ESCAPE_NSID_VMWARE;
	cmd[2] = 2 * sizeof(uint32_t);
	cmd[3] = SVGA_ESCAPE_VMWARE_VIDEO_FLUSH;
	cmd[4] = unit;
	return svga_fifo_write_cmd(cmd, sizeof(cmd));
}

// ===========================================================================
// Display topology and EDID
// ===========================================================================

int vmsvga2_get_num_displays(void)
{
	uint32_t n;

	if (!g_svga.present)
		return 0;
	if (!(g_svga.caps & SVGA_CAP_DISPLAY_TOPOLOGY))
		return 1;
	n = svga_read_reg(SVGA_REG_NUM_GUEST_DISPLAYS);
	if (n == 0 || n > 64)
		n = 1;
	return (int)n;
}

int vmsvga2_get_display_info(uint32_t index, svga_display_info_t *out)
{
	if (!g_svga.present || !out)
		return -1;
	if ((int)index >= vmsvga2_get_num_displays())
		return -1;
	if (!(g_svga.caps & SVGA_CAP_DISPLAY_TOPOLOGY) || index == 0) {
		// Single-display topology: the current mode is the layout.
		out->id = 0;
		out->primary = 1;
		out->pos_x = 0;
		out->pos_y = 0;
		out->width = g_svga.crtc.width;
		out->height = g_svga.crtc.height;
		return 0;
	}
	// Select the display, then read its geometry registers.
	svga_write_reg(SVGA_REG_DISPLAY_ID, index);
	out->id = index;
	out->primary = svga_read_reg(SVGA_REG_DISPLAY_IS_PRIMARY);
	out->pos_x = (int32_t)svga_read_reg(SVGA_REG_DISPLAY_POSITION_X);
	out->pos_y = (int32_t)svga_read_reg(SVGA_REG_DISPLAY_POSITION_Y);
	out->width = svga_read_reg(SVGA_REG_DISPLAY_WIDTH);
	out->height = svga_read_reg(SVGA_REG_DISPLAY_HEIGHT);
	return 0;
}

// Synthesize a valid 128-byte EDID 1.3 block advertising the current mode as
// the preferred (detailed) timing.  The virtual hardware exposes no EDID of
// its own; display servers still expect one for mode enumeration.
int vmsvga2_get_edid(uint8_t *buf, uint32_t len)
{
	uint8_t e[128];
	uint32_t w, h;
	uint32_t i, sum;

	if (!g_svga.present || !buf || len < 128)
		return -1;
	w = g_svga.crtc.width ? g_svga.crtc.width : 1024;
	h = g_svga.crtc.height ? g_svga.crtc.height : 768;

	kmemset(e, 0, sizeof(e));
	// Header
	e[0] = 0x00;
	e[1] = 0xFF;
	e[2] = 0xFF;
	e[3] = 0xFF;
	e[4] = 0xFF;
	e[5] = 0xFF;
	e[6] = 0xFF;
	e[7] = 0x00;
	// Manufacturer id "LKO" (compressed ASCII), product 0x0001
	e[8] = 0x32;
	e[9] = 0xCF;
	e[10] = 0x01;
	e[11] = 0x00;
	// Serial, week, year (2026 -> 36), EDID 1.3
	e[16] = 1;
	e[17] = 36;
	e[18] = 1;
	e[19] = 3;
	// Digital input, sizes unknown, gamma 2.2
	e[20] = 0x80;
	e[23] = 120 - 100;
	// Features: preferred timing mode present
	e[24] = 0x02;
	// Chromaticity left zeroed (defaults); established timings none.
	// Detailed timing #1: current mode, 60 Hz CVT-ish blanking
	{
		uint32_t hbl = 160, vbl = 45;
		uint32_t pclk_10khz =
			((w + hbl) * (h + vbl) * 60) / 10000;
		uint8_t *d = &e[54];

		d[0] = (uint8_t)(pclk_10khz & 0xFF);
		d[1] = (uint8_t)(pclk_10khz >> 8);
		d[2] = (uint8_t)(w & 0xFF);
		d[3] = (uint8_t)(hbl & 0xFF);
		d[4] = (uint8_t)(((w >> 8) << 4) | (hbl >> 8));
		d[5] = (uint8_t)(h & 0xFF);
		d[6] = (uint8_t)(vbl & 0xFF);
		d[7] = (uint8_t)(((h >> 8) << 4) | (vbl >> 8));
		d[8] = 48; // hsync offset
		d[9] = 32; // hsync width
		d[10] = (3 << 4) | 5; // vsync offset/width
		d[17] = 0x1E; // digital separate sync, +h +v
	}
	// Descriptor #2: display name
	{
		static const char name[] = "LikeOS SVGA2";
		uint8_t *d = &e[72];
		d[3] = 0xFC;
		for (i = 0; i < 12 && name[i]; i++)
			d[5 + i] = (uint8_t)name[i];
		if (i < 12)
			d[5 + i] = 0x0A;
	}
	// Descriptors #3/#4: dummy
	e[90 + 3] = 0x10;
	e[108 + 3] = 0x10;
	// Checksum
	sum = 0;
	for (i = 0; i < 127; i++)
		sum += e[i];
	e[127] = (uint8_t)(0x100 - (sum & 0xFF)) & 0xFF;

	kmemcpy(buf, e, 128);
	return 0;
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
//      EAX (the same channel kernel/dev/gpu/vmwgfx/vmw_msg.c speaks for
//      RPCI, here in its simplest single-command form).  Command 17 returns
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

#define SVGA_BACKDOOR_PORT 0x5658
#define SVGA_BACKDOOR_MAGIC 0x564D5868U /* 'VMXh' */
#define SVGA_BACKDOOR_GETSCREENSIZE 17

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
	uint32_t eax = SVGA_BACKDOOR_MAGIC;
	uint32_t ebx = 0;
	uint32_t ecx = SVGA_BACKDOOR_GETSCREENSIZE;
	uint32_t edx = SVGA_BACKDOOR_PORT;

	__asm__ __volatile__("inl %%dx, %%eax"
			     : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx)
			     :
			     : "memory");

	// All ones: the protocol's refusal, and also what an unclaimed port
	// reads back.  The magic returned unchanged: a hypervisor that does
	// not implement this command.
	if (eax == 0xFFFFFFFFU || eax == SVGA_BACKDOOR_MAGIC)
		return -1;
	*w = eax >> 16;
	*h = eax & 0xFFFFU;
	return 0;
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

static int svga_fifo_init(void)
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

int vmsvga2_init(void)
{
	const pci_device_t *dev;
	uint32_t cmd_reg;
	uint32_t bar0, bar1;

	BUILD_BUG_ON(sizeof(svga3d_cmd_header_t) != 8);
	BUILD_BUG_ON(sizeof(svga_guest_mem_descriptor_t) != 8);

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

	g_svga.caps = svga_read_reg(SVGA_REG_CAPABILITIES);
	g_svga.vram_size = svga_read_reg(SVGA_REG_VRAM_SIZE);
	g_svga.max_width = svga_read_reg(SVGA_REG_MAX_WIDTH);
	g_svga.max_height = svga_read_reg(SVGA_REG_MAX_HEIGHT);
	g_svga.host_bpp = svga_read_reg(SVGA_REG_HOST_BITS_PER_PIXEL);
	g_svga.fb_size = svga_read_reg(SVGA_REG_FB_SIZE);
	svga_write_reg(SVGA_REG_GUEST_ID, SVGA_GUEST_ID_OTHER);

	if (WARN_ON(g_svga.vram_size == 0))
		return -1;

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

	// Bounce buffer for FIFO reservations that wrap the ring.
	g_svga.bounce_size = 16 * 1024;
	g_svga.bounce = kalloc(g_svga.bounce_size);
	if (!g_svga.bounce)
		return -1;

	if (svga_fifo_init() != 0) {
		kfree(g_svga.bounce);
		g_svga.bounce = NULL;
		return -1;
	}

	g_svga.next_fence = 0;
	g_svga.present = 1;
	g_vmsvga_initialized = 1;

	// Fence-completion interrupts (capability-gated; QEMU: polling).
	svga_irq_init();

	kprintf("svga2: SVGA II v%u io=0x%x fb=0x%lx vram=%uKB fifo=%uKB\n",
		g_svga.version, g_svga.io_base,
		(unsigned long)g_svga.fb_phys, g_svga.vram_size / 1024,
		g_svga.fifo_size / 1024);
	kprintf("svga2: caps=0x%x fifo_caps=0x%x max=%ux%u host_bpp=%u\n",
		g_svga.caps, g_svga.fifo_caps, g_svga.max_width,
		g_svga.max_height, g_svga.host_bpp);
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
// The bounds are the ones the reference driver applies to this device: the
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
	if ((uint64_t)w * h * 4 > g_svga.vram_size)
		return "more video memory than the adapter has";
	return NULL;
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
	// The standard mode list, largest first: the same set the reference
	// driver offers on a connector of this device, rather than the
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
	if (w == 0) {
		// Nothing in the list fits (tiny VRAM, or a ceiling below every
		// entry): last resort, and small enough that the ceiling is not
		// worth honouring at the price of having no display at all.
		w = 640;
		h = 480;
		if (w > g_svga.max_width || h > g_svga.max_height ||
		    (uint64_t)w * h * 4 > g_svga.vram_size) {
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
		// Roll back: stop forwarding updates, disable the device and
		// leave the GOP framebuffer path untouched.
		fb_set_flush_hook(0);
		g_svga.active = 0;
		svga_write_reg(SVGA_REG_ENABLE, 0);
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
	svga_report_errors();

	kprintf("svga2: display %ux%ux%u pitch=%u from %s (%s)\n",
		g_svga.crtc.width, g_svga.crtc.height, g_svga.crtc.bpp,
		g_svga.crtc.pitch, source,
		svga_has_fence() ? "fenced" : "sync-only");
}

void vmsvga2_shutdown(void)
{
	if (!g_svga.present)
		return;
	vmsvga2_fifo_flush();
	svga_write_reg(SVGA_REG_ENABLE, 0);
	g_svga.active = 0;
	g_svga.crtc.enabled = 0;
}

// ===========================================================================
// Exports for the display-manager (DRM-style) driver in kernel/dev/gpu.
//
// That driver owns the device's 3D/KMS interface to userspace; this file
// keeps owning the hardware access primitives (register file, FIFO, fence
// IRQ) and the boot-time console framebuffer.  The two meet here.
// ===========================================================================

#include <kernel/dev/video/vmsvga2_hw.h>
#include <kernel/dev/video/fbdev.h>

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

/* One command of `bytes' from `data', atomically. */
int vmsvga2_hw_fifo_submit(const void *data, uint32_t bytes)
{
	if (!g_svga.present || !g_svga.fifo)
		return -1;
	if (bytes & 3)
		return -1;
	if (bytes > g_svga.bounce_size && bytes > g_svga.fifo_size / 2)
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
	if (bytes > g_svga.bounce_size && bytes > g_svga.fifo_size / 2)
		return -1;
	return svga_fifo_write_cmd_quiet(data, bytes);
}

/* A reservation the caller fills in place; must be followed by a commit
 * of the same size with the lock still held.  The lock is taken here and
 * released by the commit (or the abort). */
static uint64_t g_hw_fifo_flags;

void *vmsvga2_hw_fifo_reserve(uint32_t bytes)
{
	void *dst;

	if (!g_svga.present || !g_svga.fifo || (bytes & 3))
		return NULL;
	if (bytes > g_svga.bounce_size && bytes > g_svga.fifo_size / 2)
		return NULL;
	spin_lock_irqsave(&svga_fifo_lock, &g_hw_fifo_flags);
	dst = svga_fifo_reserve(bytes);
	if (!dst)
		spin_unlock_irqrestore(&svga_fifo_lock, g_hw_fifo_flags);
	return dst;
}

void vmsvga2_hw_fifo_commit(uint32_t bytes)
{
	svga_fifo_commit(bytes);
	spin_unlock_irqrestore(&svga_fifo_lock, g_hw_fifo_flags);
	svga_doorbell();
}

void vmsvga2_hw_fifo_abort(void)
{
	g_svga.reserved_bytes = 0;
	g_svga.reserved_in_place = 0;
	spin_unlock_irqrestore(&svga_fifo_lock, g_hw_fifo_flags);
}

void vmsvga2_hw_doorbell(void)
{
	svga_doorbell();
}

uint32_t vmsvga2_hw_fence_current(void)
{
	return svga_has_fence() ? g_svga.fifo[SVGA_FIFO_FENCE] : 0;
}

int vmsvga2_hw_has_fence(void)
{
	return svga_has_fence();
}

void vmsvga2_hw_set_fence_goal(uint32_t goal)
{
	if (!g_svga.irq_enabled || !svga_has_fifo_reg(SVGA_FIFO_FENCE_GOAL))
		return;
	g_svga.fifo[SVGA_FIFO_FENCE_GOAL] = goal;
	svga_write_reg(SVGA_REG_IRQMASK,
		       SVGA_IRQ_BASE_MASK | SVGA_IRQFLAG_ANY_FENCE |
			       SVGA_IRQFLAG_FENCE_GOAL);
	svga_doorbell();
}

/* The fence interrupt: one consumer beyond this file. */
static vmsvga2_hw_irq_cb_t g_hw_irq_cb;

void vmsvga2_hw_set_irq_callback(vmsvga2_hw_irq_cb_t cb)
{
	g_hw_irq_cb = cb;
}

/* Called from vmsvga2_irq() after the status has been acknowledged. */
static void vmsvga2_hw_irq_notify(uint32_t status)
{
	vmsvga2_hw_irq_cb_t cb = g_hw_irq_cb;

	if (cb)
		cb(status);
}

/* Display ownership hand-over.  While the display-manager driver's master
 * holds the screen the console must not paint (see fbdev_display_owned);
 * on release the console is redrawn in full and the boot mode restored. */
static int g_hw_display_taken;

void vmsvga2_hw_display_take(void)
{
	if (g_hw_display_taken)
		return;
	g_hw_display_taken = 1;
	fbdev_opened();
}

void vmsvga2_hw_display_release(void)
{
	if (!g_hw_display_taken)
		return;
	g_hw_display_taken = 0;
	svga_write_reg(SVGA_REG_TRACES, 1);
	fbdev_closed();
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
	return vmsvga2_set_mode(width, height, 32);
}

/* Mode set for the display-manager: the console's framebuffer geometry
 * follows (console_reinit_framebuffer), as the fbdev ioctl path does. */
int vmsvga2_hw_set_mode(uint32_t width, uint32_t height)
{
	framebuffer_info_t fi;
	int rc = vmsvga2_set_mode(width, height, 32);

	if (rc != 0)
		return rc;
	if (vmsvga2_get_info(&fi) != 0)
		return -1;
	/* Cascade the new geometry through fb/console/tty (SIGWINCH). */
	if (console_reinit_framebuffer(&fi) != 0)
		return -1;
	return 0;
}
