// LikeOS -- interrupt registration and dispatch.
//
// Two layers live here.
//
// The legacy layer (irq_request_vector/gsi/msi/msix, irq_alloc_vector):
// one table indexed by IDT vector, shared by every CPU, used by the
// drivers that existed before the interrupt core and kept exactly as it
// was, dynamic vectors 63..127 included.  The vector matrix of the
// interrupt core covers 128..239 only; a vector of an I/O APIC line that
// falls into that range is taken there on every CPU the moment the line
// is routed (irq_reserve_legacy_vector), so the two layers can never
// collide.
//
// I/O APIC lines: each GSI has ONE vector, recorded in g_gsi_vector the
// first time the line is routed (by irq_request_gsi, or by a driver that
// routes its line itself through ioapic_configure_legacy_irq) and kept
// for good; every later user of a shared line gets the same vector.  The
// choice (kernel/ke/irq_gsi.c): GSIs 0..25 keep their natural vector
// 32 + GSI -- the legacy INTx arms of interrupt.c recognise their line by
// it -- unless that vector has a fixed meaning (32 LAPIC timer, 47 LAPIC
// error, 58 SCI, 59-61 NIC MSI, 62 HPET), which no line may share: a
// handled line interrupt skips the fixed arm.  Those GSIs, and every GSI
// above 25, get a free vector of the dynamic range instead, which the
// MSI allocator then no longer hands out.  A line moved off its natural
// vector is reported once ("irq: GSI n on vector v").
//
// The interrupt core: interrupt NUMBERS (IRQ_NR_FIRST + descriptor index)
// for request_irq()/free_irq(), each with an affinity mask, a source
// (irq_chip_ops: compose the message, mask, unmask) and up to
// IRQ_DESC_MAX_ACTIONS handlers.  Its vector is allocated per CPU from
// the matrix (kernel/ke/irq_matrix.c) when the first handler is requested
// ("startup") and released with the last ("shutdown"); a MANAGED
// descriptor holds a reservation on every CPU of its mask from creation
// on, so its startup cannot fail.  Each CPU has its own vector -> desc
// table (g_vector_irq), so 64 CPUs can each carry dozens of queue
// interrupts on the same vector numbers.
//
// Flow (the edge flow, MSI/MSI-X): acknowledge the local APIC first, then
// run the handlers; an interrupt that arrives while the descriptor is
// disabled or already running on another CPU is marked pending and
// replayed (disabled: on enable; running: by the running CPU before it
// lets go).  INTx descriptors ride on the legacy table through a shared
// trampoline (level flow: the legacy dispatcher acknowledges once a
// handler claimed the line).  A handler may return IRQ_WAKE_THREAD to
// run the action's thread function in an interrupt thread (process
// context); with IRQF_ONESHOT the source stays masked until it returns.
//
// Locks: g_vector_lock (spin, irqsave) protects the matrix and every
// vector table; desc->lock (spin, irqsave) the descriptor's flow state
// and action list; desc->req_mutex serialises request/free/affinity of
// one descriptor (sleeping).  Order: req_mutex -> g_vector_lock ->
// desc->lock.  The dispatcher takes only desc->lock.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/ioapic.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ==========================================================================
 * Vector matrix
 * ========================================================================== */

static struct irq_matrix g_matrix;
static spinlock_t g_vector_lock = SPINLOCK_INIT("irq_vector");
static volatile int g_matrix_ready;

/* (cpu, vector) -> descriptor index + 1 (0: none).  Written under
 * g_vector_lock, read locklessly by the dispatcher. */
static volatile uint16_t g_vector_irq[MAX_CPUS][256];

static void irq_matrix_init_locked(void)
{
	lockdep_assert_held(&g_vector_lock);
	if (g_matrix_ready)
		return;
	BUILD_BUG_ON(IRQ_DYN_VECTOR_LAST >= IRQ_MATRIX_FIRST);
	BUILD_BUG_ON(IRQ_MATRIX_END > IPI_BLOCK_SOFTIRQ_VECTOR);
	irq_matrix_setup(&g_matrix, IRQ_MATRIX_FIRST, IRQ_MATRIX_END,
			 IRQ_MATRIX_SEARCH_START);
	__atomic_store_n(&g_matrix_ready, 1, __ATOMIC_RELEASE);
}

void irq_core_init(void)
{
	uint64_t fl;

	spin_lock_irqsave(&g_vector_lock, &fl);
	irq_matrix_init_locked();
	spin_unlock_irqrestore(&g_vector_lock, fl);
}

/* ==========================================================================
 * Legacy layer: one shared table indexed by vector
 * ========================================================================== */

#define IRQ_MAX_ACTIONS 4

struct irq_action {
	irq_handler_t fn;
	void *arg;
	const char *name;
};

struct irq_vdesc {
	struct irq_action act[IRQ_MAX_ACTIONS];
	int nact;
	int allocated; /* handed out by irq_alloc_vector(), or a GSI's */
	int gsi; /* GSI + 1 of the I/O APIC line routed here, 0: none */
	uint64_t count;
};

static struct irq_vdesc g_irq[256];
static spinlock_t g_irq_lock = SPINLOCK_INIT("irq");
/* GSI -> vector, 0: not routed (g_irq_lock). */
static uint8_t g_gsi_vector[IRQ_GSI_MAX];

/* The fixed vectors irq_gsi.c keeps lines away from are their owners'. */
BUILD_BUG_ON(IRQ_VEC_LAPIC_TIMER != LAPIC_TIMER_VECTOR);
BUILD_BUG_ON(IRQ_VEC_LAPIC_ERROR != LAPIC_ERROR_VECTOR);
BUILD_BUG_ON(IRQ_VEC_ACPI_SCI != ACPI_SCI_VECTOR);
BUILD_BUG_ON(IRQ_VEC_E1000_MSI != E1000_MSI_VECTOR);
BUILD_BUG_ON(IRQ_VEC_E1000E_MSI != E1000E_MSI_VECTOR);
BUILD_BUG_ON(IRQ_VEC_VMXNET3_MSI != VMXNET3_MSI_VECTOR);
BUILD_BUG_ON(IRQ_VEC_HRTIMER != HRTIMER_VECTOR);
BUILD_BUG_ON(32 + IRQ_GSI_NATURAL_LAST >= IRQ_VEC_ACPI_SCI);

int irq_alloc_vector(void)
{
	uint64_t fl;

	spin_lock_irqsave(&g_irq_lock, &fl);
	for (int v = IRQ_DYN_VECTOR_FIRST; v <= IRQ_DYN_VECTOR_LAST; v++) {
		if (!g_irq[v].allocated && g_irq[v].nact == 0 &&
		    !g_irq[v].gsi) {
			g_irq[v].allocated = 1;
			spin_unlock_irqrestore(&g_irq_lock, fl);
			return v;
		}
	}
	spin_unlock_irqrestore(&g_irq_lock, fl);
	return -1;
}

void irq_free_vector(int vector)
{
	uint64_t fl;

	if (vector < 0 || vector > 255)
		return;
	spin_lock_irqsave(&g_irq_lock, &fl);
	/* A line's vector stays with the line. */
	if (!WARN_ON_ONCE(g_irq[vector].gsi != 0))
		g_irq[vector].allocated = 0;
	spin_unlock_irqrestore(&g_irq_lock, fl);
}

/* Is `v' unavailable for a GSI that has no vector yet (g_irq_lock). */
static int gsi_vector_busy_locked(int v, void *ctx)
{
	(void)ctx;
	lockdep_assert_held(&g_irq_lock);
	return g_irq[v].allocated || g_irq[v].nact > 0 || g_irq[v].gsi != 0;
}

/* Record the line's vector (g_irq_lock). */
static void gsi_route_record_locked(int gsi, int v)
{
	lockdep_assert_held(&g_irq_lock);
	g_gsi_vector[gsi] = (uint8_t)v;
	g_irq[v].gsi = gsi + 1;
	/* A dynamic vector is the line's now: the MSI allocator skips it. */
	if (v >= IRQ_DYN_VECTOR_FIRST && v <= IRQ_DYN_VECTOR_LAST)
		g_irq[v].allocated = 1;
}

/* The vector of `gsi', chosen now if the line has none.  -errno. */
static int irq_gsi_vector_assign(int gsi)
{
	uint64_t fl;
	int v, chosen = 0;

	if (gsi < 0 || gsi >= IRQ_GSI_MAX)
		return -EINVAL;
	spin_lock_irqsave(&g_irq_lock, &fl);
	v = g_gsi_vector[gsi];
	if (!v) {
		v = irq_gsi_choose_vector(gsi, gsi_vector_busy_locked, NULL);
		if (v > 0) {
			gsi_route_record_locked(gsi, v);
			chosen = 1;
		}
	}
	spin_unlock_irqrestore(&g_irq_lock, fl);
	if (v <= 0) {
		kprintf("irq: no free vector for GSI %d\n", gsi);
		return -ENOSPC;
	}
	/* A line some fixed-vector owner routed itself: only the SCI's (a
	 * level line, whose arm runs whenever no handler claimed the
	 * interrupt) can be shared.  On an edge source's vector -- the HPET
	 * comparator -- a handled line interrupt would swallow its edge. */
	if (irq_vector_is_fixed(v) && v != IRQ_VEC_ACPI_SCI) {
		kprintf("irq: GSI %d is routed to fixed vector %d, not shared\n",
			gsi, v);
		return -EBUSY;
	}
	if (chosen && v != 32 + gsi)
		kprintf("irq: GSI %d on vector %d (%s)\n", gsi, v,
			gsi > IRQ_GSI_NATURAL_LAST ?
				"above the natural range" :
			irq_vector_is_fixed(32 + gsi) ?
				"vector 32 + GSI is a fixed one" :
				"vector 32 + GSI is taken");
	return v;
}

int irq_gsi_vector(int gsi)
{
	uint64_t fl;
	int v;

	if (gsi < 0 || gsi >= IRQ_GSI_MAX)
		return -1;
	spin_lock_irqsave(&g_irq_lock, &fl);
	v = g_gsi_vector[gsi];
	spin_unlock_irqrestore(&g_irq_lock, fl);
	return v ? v : -1;
}

void irq_note_gsi_route(int gsi, int vector)
{
	uint64_t fl;
	int old;

	if (gsi < 0 || gsi >= IRQ_GSI_MAX || vector < 32 || vector > 255)
		return;
	spin_lock_irqsave(&g_irq_lock, &fl);
	old = g_gsi_vector[gsi];
	if (old != vector) {
		if (old && g_irq[old].gsi == gsi + 1) {
			g_irq[old].gsi = 0;
			/* A dynamic vector nobody listens on goes back. */
			if (old >= IRQ_DYN_VECTOR_FIRST &&
			    old <= IRQ_DYN_VECTOR_LAST && g_irq[old].nact == 0)
				g_irq[old].allocated = 0;
		}
		gsi_route_record_locked(gsi, vector);
	}
	spin_unlock_irqrestore(&g_irq_lock, fl);
	/* A second router of the line with a vector of its own: the entry
	 * points at the new one, whoever listens on the old one is cut off. */
	if (old && old != vector)
		kprintf("irq: GSI %d moved from vector %d to %d\n", gsi, old,
			vector);
}

int irq_request_vector(int vector, irq_handler_t fn, void *arg,
		       const char *name)
{
	uint64_t fl;

	if (vector < 32 || vector > 255 || !fn)
		return -EINVAL;
	spin_lock_irqsave(&g_irq_lock, &fl);
	struct irq_vdesc *d = &g_irq[vector];
	if (d->nact >= IRQ_MAX_ACTIONS) {
		spin_unlock_irqrestore(&g_irq_lock, fl);
		return -EBUSY;
	}
	d->act[d->nact].fn = fn;
	d->act[d->nact].arg = arg;
	d->act[d->nact].name = name;
	/* irq_dispatch reads the actions without the lock, on any CPU: the
	 * entry is complete before the count that makes it reachable (an
	 * optimising compiler stored the count first). */
	__asm__ volatile("" ::: "memory");
	d->nact++;
	spin_unlock_irqrestore(&g_irq_lock, fl);
	return 0;
}

void irq_free(int vector, irq_handler_t fn, void *arg)
{
	uint64_t fl;

	if (vector < 0 || vector > 255)
		return;
	spin_lock_irqsave(&g_irq_lock, &fl);
	struct irq_vdesc *d = &g_irq[vector];
	for (int i = 0; i < d->nact; i++) {
		if (d->act[i].fn == fn && d->act[i].arg == arg) {
			for (int j = i + 1; j < d->nact; j++)
				d->act[j - 1] = d->act[j];
			d->nact--;
			break;
		}
	}
	spin_unlock_irqrestore(&g_irq_lock, fl);
}

static int irq_vector_nact(int vector)
{
	return __atomic_load_n(&g_irq[vector].nact, __ATOMIC_ACQUIRE);
}

int irq_reserve_legacy_vector(int vector)
{
	uint64_t fl;
	int rc;

	if (vector < 32 || vector > 255)
		return -EINVAL;
	spin_lock_irqsave(&g_vector_lock, &fl);
	irq_matrix_init_locked();
	rc = irq_matrix_assign_legacy(&g_matrix, (unsigned int)vector);
	spin_unlock_irqrestore(&g_vector_lock, fl);
	if (rc)
		kprintf("irq: vector %d of an I/O APIC line is in use by a per-CPU interrupt (%d)\n",
			vector, rc);
	return rc;
}

int irq_request_gsi(int gsi, irq_handler_t fn, void *arg, const char *name,
		    int active_low, int level)
{
	int vector, rc;

	if (gsi < 0 || gsi >= IRQ_GSI_MAX)
		return -EINVAL;
	vector = irq_gsi_vector_assign(gsi);
	if (vector < 0)
		return vector;
	rc = irq_request_vector(vector, fn, arg, name);
	if (rc)
		return rc;
	if (ioapic_configure_legacy_irq((uint8_t)gsi, (uint8_t)vector,
					active_low ? IOAPIC_POLARITY_LOW :
						     IOAPIC_POLARITY_HIGH,
					level ? IOAPIC_TRIGGER_LEVEL :
						IOAPIC_TRIGGER_EDGE) != 0) {
		irq_free(vector, fn, arg);
		return -EIO;
	}
	return 0;
}

static uint32_t cpu_apic_id(int cpu)
{
	if (cpu <= 0)
		return lapic_get_id_cpuid();
	percpu_t *pc = percpu_get(cpu);
	return pc ? pc->apic_id : lapic_get_id_cpuid();
}

int irq_request_msi(const struct pci_device *dev, int cpu, irq_handler_t fn,
		    void *arg, const char *name, int *vector_out)
{
	int vector = irq_alloc_vector();

	if (vector < 0)
		return -ENOSPC;
	int rc = irq_request_vector(vector, fn, arg, name);
	if (rc) {
		irq_free_vector(vector);
		return rc;
	}
	if (pci_enable_msi_cpu(dev, (uint8_t)vector, cpu_apic_id(cpu)) != 0) {
		irq_free(vector, fn, arg);
		irq_free_vector(vector);
		return -EIO;
	}
	if (vector_out)
		*vector_out = vector;
	return 0;
}

int irq_request_msix(const struct pci_device *dev, int entry, int cpu,
		     irq_handler_t fn, void *arg, const char *name,
		     int *vector_out)
{
	int vector = irq_alloc_vector();

	if (vector < 0)
		return -ENOSPC;
	int rc = irq_request_vector(vector, fn, arg, name);
	if (rc) {
		irq_free_vector(vector);
		return rc;
	}
	if (pci_enable_msix(dev, entry, (uint8_t)vector, cpu_apic_id(cpu)) !=
	    0) {
		irq_free(vector, fn, arg);
		irq_free_vector(vector);
		return -EIO;
	}
	if (vector_out)
		*vector_out = vector;
	return 0;
}

static int irq_dispatch_legacy(uint64_t vector)
{
	struct irq_vdesc *d = &g_irq[vector];

	/* No lock on the fast path: actions are appended, never moved while
	 * live, and removal is rare and racing it costs at most one stale
	 * call into a handler that answers "not mine".  The count is read
	 * once, with acquire, so the entries are read after it. */
	int n = __atomic_load_n(&d->nact, __ATOMIC_ACQUIRE);

	if (n == 0)
		return 0;
	int handled = 0;
	for (int i = 0; i < n; i++) {
		struct irq_action *a = &d->act[i];
		if (a->fn && a->fn(a->arg))
			handled = 1;
	}
	if (handled) {
		d->count++;
		lapic_eoi();
	}
	return handled;
}

/* The APIC ID of a CPU for the interrupt core: the CPU's own when it is
 * the caller (always right, also before SMP set-up filled the per-CPU
 * record), the per-CPU record otherwise. */
static uint32_t irq_cpu_apic_id(unsigned int cpu)
{
	percpu_t *pc;

	if (cpu == this_cpu_id())
		return lapic_get_id_cpuid();
	pc = percpu_get(cpu);
	return pc ? pc->apic_id : lapic_get_id_cpuid();
}

/* ==========================================================================
 * Interrupt core: descriptors
 * ========================================================================== */

#define IRQ_DESC_MAX_ACTIONS 4
#define IRQ_THREAD_STACK 16384
/* How long an affinity move keeps the old vector live for messages that
 * were already on their way. */
#define IRQ_MOVE_CLEANUP_MS 10

struct irq_thread;

struct irq_naction {
	irq_handler_t handler; /* primary (hard IRQ) */
	irq_handler_t thread_fn; /* NULL: not threaded */
	void *arg;
	const char *name;
	unsigned long flags;
	struct irq_thread *thread;
	unsigned int thread_bit; /* in desc->threads_oneshot */
};

enum irq_desc_kind {
	IRQ_KIND_VECTOR = 0, /* vector allocated at startup */
	IRQ_KIND_GSI, /* I/O APIC line through the legacy table */
	IRQ_KIND_BLOCK, /* multi-message MSI: vector fixed at creation */
	IRQ_KIND_VIRTUAL, /* no vector: delivered through a parent irq */
};

struct irq_desc {
	spinlock_t lock;
	struct mutex req_mutex;
	int irq; /* IRQ_NR_FIRST + index */
	int in_use;
	enum irq_desc_kind kind;
	const struct irq_chip_ops *ops;
	void *data;
	cpumask_t affinity;
	int managed;
	int reserved; /* regular: counted in global_reserved */
	/* Delivery target while started (cpu, vector), -1 when none. */
	int cpu;
	int vector;
	uint32_t apic_id;
	int started;
	int no_move; /* block member: one shared message */
	/* GSI route */
	int gsi, gsi_active_low, gsi_level;
	/* Flow state (desc->lock). */
	int depth; /* disable_irq nesting */
	int masked; /* the source is masked now */
	int pending; /* arrived while disabled/running */
	int running; /* handlers executing on some CPU */
	uint32_t threads_oneshot; /* threads holding the oneshot mask */
	volatile int threads_active; /* thread functions queued/running */
	struct wait_queue_head wait_for_threads;
	struct irq_naction act[IRQ_DESC_MAX_ACTIONS];
	int nact;
	const char *name;
	/* Statistics and unhandled-interrupt detection (desc->lock) */
	volatile uint64_t count;
	struct irq_spurious spur;
	volatile uint32_t threads_handled; /* thread_fn returned IRQ_HANDLED */
	uint32_t spurious_disabled;
	/* Trigger programmed through irq_set_irq_type (IRQ_TYPE_*), and the
	 * wake-up nesting of irq_set_irq_wake (desc->lock). */
	unsigned int trigger;
	unsigned int wake_depth;
	struct irq_desc *free_next;
};

static struct irq_desc *g_descs[IRQ_NR_MAX];
static spinlock_t g_desc_lock = SPINLOCK_INIT("irq_desc");
static struct irq_desc *g_desc_free_head, *g_desc_free_tail;
static unsigned int g_desc_next; /* never-used indexes start here */

static inline struct irq_desc *irq_to_desc(int irq)
{
	struct irq_desc *d;

	if (irq < IRQ_NR_FIRST || irq >= IRQ_NR_FIRST + IRQ_NR_MAX)
		return NULL;
	d = __atomic_load_n(&g_descs[irq - IRQ_NR_FIRST], __ATOMIC_ACQUIRE);
	if (!d || !__atomic_load_n(&d->in_use, __ATOMIC_ACQUIRE))
		return NULL;
	return d;
}

/* A descriptor: a freed one (oldest first, so a stale delivery to a
 * just-freed number is unlikely to meet a new owner) or a new one. */
static struct irq_desc *desc_get(void)
{
	struct irq_desc *d = NULL;
	unsigned int idx = IRQ_NR_MAX;
	uint64_t fl;

	spin_lock_irqsave(&g_desc_lock, &fl);
	d = g_desc_free_head;
	if (d) {
		g_desc_free_head = d->free_next;
		if (!g_desc_free_head)
			g_desc_free_tail = NULL;
		d->free_next = NULL;
	} else if (g_desc_next < IRQ_NR_MAX) {
		idx = g_desc_next++;
	}
	spin_unlock_irqrestore(&g_desc_lock, fl);
	if (d)
		return d;
	if (idx >= IRQ_NR_MAX)
		return NULL;
	d = kcalloc(1, sizeof(*d));
	if (!d)
		return NULL; /* the index is lost; harmless (4096 of them) */
	spinlock_init(&d->lock, "irq_desc");
	mutex_init(&d->req_mutex, "irq_req");
	wq_head_init(&d->wait_for_threads, "irq_threads");
	d->irq = IRQ_NR_FIRST + (int)idx;
	__atomic_store_n(&g_descs[idx], d, __ATOMIC_RELEASE);
	return d;
}

static void desc_put(struct irq_desc *d)
{
	uint64_t fl;

	spin_lock_irqsave(&g_desc_lock, &fl);
	d->free_next = NULL;
	if (g_desc_free_tail)
		g_desc_free_tail->free_next = d;
	else
		g_desc_free_head = d;
	g_desc_free_tail = d;
	spin_unlock_irqrestore(&g_desc_lock, fl);
}

/* Reset a descriptor taken from desc_get() to a fresh one. */
static void desc_reset(struct irq_desc *d, const struct irq_chip_ops *ops,
		       void *data, enum irq_desc_kind kind)
{
	d->kind = kind;
	d->ops = ops;
	d->data = data;
	cpumask_clear(&d->affinity);
	d->managed = 0;
	d->reserved = 0;
	d->cpu = -1;
	d->vector = -1;
	d->apic_id = 0;
	d->started = 0;
	d->no_move = 0;
	d->gsi = -1;
	d->depth = 0;
	d->masked = 0;
	d->pending = 0;
	d->running = 0;
	d->threads_oneshot = 0;
	d->threads_active = 0;
	d->nact = 0;
	__builtin_memset(d->act, 0, sizeof(d->act));
	d->name = NULL;
	d->count = 0;
	__builtin_memset(&d->spur, 0, sizeof(d->spur));
	d->threads_handled = 0;
	d->spurious_disabled = 0;
	d->trigger = IRQ_TYPE_NONE;
	d->wake_depth = 0;
}

/* The CPUs a message can be aimed at: online, with an APIC ID that fits
 * the 8-bit destination field of a message address. */
static void irq_target_cpus(cpumask_t *out)
{
	unsigned int n = cpumask_nr_online();

	cpumask_clear(out);
	for (unsigned int c = 0; c < n; c++)
		if (irq_cpu_apic_id(c) < 256)
			cpumask_set_cpu(c, out);
	if (cpumask_empty(out))
		cpumask_set_cpu(0, out);
}

int irq_alloc_desc(const struct irq_chip_ops *ops, void *data,
		   const cpumask_t *affinity, int managed)
{
	struct irq_desc *d;
	cpumask_t targets, msk;
	uint64_t fl;
	int rc = 0;

	might_sleep();
	d = desc_get();
	if (!d)
		return -ENOMEM;
	desc_reset(d, ops, data, IRQ_KIND_VECTOR);
	irq_target_cpus(&targets);
	if (affinity && cpumask_and(&msk, affinity, &targets))
		cpumask_copy(&d->affinity, &msk);
	else
		cpumask_copy(&d->affinity, &targets);
	d->managed = managed ? 1 : 0;

	spin_lock_irqsave(&g_vector_lock, &fl);
	irq_matrix_init_locked();
	if (d->managed) {
		/* Managed: a bit on every CPU of the mask now, so the
		 * vector can be had later without failing. */
		rc = irq_matrix_reserve_managed(&g_matrix, &d->affinity);
	} else {
		irq_matrix_reserve(&g_matrix);
		d->reserved = 1;
	}
	spin_unlock_irqrestore(&g_vector_lock, fl);
	if (rc) {
		desc_put(d);
		return rc;
	}
	__atomic_store_n(&d->in_use, 1, __ATOMIC_RELEASE);
	return d->irq;
}

int irq_alloc_desc_gsi(const struct irq_chip_ops *ops, void *data, int gsi,
		       int active_low, int level)
{
	struct irq_desc *d;
	int vector;

	might_sleep();
	if (gsi < 0 || gsi >= IRQ_GSI_MAX)
		return -EINVAL;
	vector = irq_gsi_vector_assign(gsi);
	if (vector < 0)
		return vector;
	d = desc_get();
	if (!d)
		return -ENOMEM;
	desc_reset(d, ops, data, IRQ_KIND_GSI);
	/* The I/O APIC line is routed to the boot CPU (the legacy table's
	 * configuration); the mask states that. */
	cpumask_set_cpu(0, &d->affinity);
	d->gsi = gsi;
	d->gsi_active_low = active_low;
	d->gsi_level = level;
	d->no_move = 1;
	d->vector = vector; /* the line's, for good (irq_gsi_vector) */
	d->cpu = 0;
	__atomic_store_n(&d->in_use, 1, __ATOMIC_RELEASE);
	return d->irq;
}

int irq_alloc_desc_virtual(const struct irq_chip_ops *ops, void *data,
			   const cpumask_t *affinity, int managed)
{
	struct irq_desc *d;
	cpumask_t targets, msk;

	might_sleep();
	d = desc_get();
	if (!d)
		return -ENOMEM;
	desc_reset(d, ops, data, IRQ_KIND_VIRTUAL);
	irq_target_cpus(&targets);
	if (affinity && cpumask_and(&msk, affinity, &targets))
		cpumask_copy(&d->affinity, &msk);
	else
		cpumask_copy(&d->affinity, &targets);
	d->managed = managed ? 1 : 0;
	d->no_move = 1;
	__atomic_store_n(&d->in_use, 1, __ATOMIC_RELEASE);
	return d->irq;
}

int irq_alloc_desc_block(const struct irq_chip_ops *ops, void *const *data,
			 unsigned int n, const cpumask_t *affinity, int *irqs)
{
	struct irq_desc *d[32];
	cpumask_t targets, msk;
	unsigned int cpu = 0, i;
	uint64_t fl;
	int base;

	might_sleep();
	if (!ops || !data || !irqs || n == 0 || n > 32 || (n & (n - 1)))
		return -EINVAL;
	for (i = 0; i < n; i++) {
		d[i] = desc_get();
		if (!d[i]) {
			while (i--)
				desc_put(d[i]);
			return -ENOMEM;
		}
		desc_reset(d[i], ops, data[i], IRQ_KIND_BLOCK);
		d[i]->no_move = 1;
	}
	irq_target_cpus(&targets);
	if (!affinity || !cpumask_and(&msk, affinity, &targets))
		cpumask_copy(&msk, &targets);
	spin_lock_irqsave(&g_vector_lock, &fl);
	irq_matrix_init_locked();
	base = irq_matrix_alloc_block(&g_matrix, &msk, n, &cpu);
	if (base < 0 && !cpumask_equal(&msk, &targets))
		base = irq_matrix_alloc_block(&g_matrix, &targets, n, &cpu);
	if (base >= 0) {
		for (i = 0; i < n; i++) {
			d[i]->cpu = (int)cpu;
			d[i]->vector = base + (int)i;
			d[i]->apic_id = irq_cpu_apic_id(cpu);
			cpumask_clear(&d[i]->affinity);
			cpumask_set_cpu(cpu, &d[i]->affinity);
			__atomic_store_n(&g_vector_irq[cpu][base + (int)i],
					 (uint16_t)(d[i]->irq - IRQ_NR_FIRST +
						    1),
					 __ATOMIC_RELEASE);
		}
	}
	spin_unlock_irqrestore(&g_vector_lock, fl);
	if (base < 0) {
		for (i = 0; i < n; i++)
			desc_put(d[i]);
		return base;
	}
	for (i = 0; i < n; i++) {
		/* Masked until a handler is requested. */
		d[i]->masked = 1;
		__atomic_store_n(&d[i]->in_use, 1, __ATOMIC_RELEASE);
		irqs[i] = d[i]->irq;
	}
	/* The message (base vector, target) is fixed now. */
	for (i = 0; i < n; i++)
		if (ops->compose_msg)
			ops->compose_msg(d[i]->irq, d[i]->data,
					 (uint8_t)d[i]->vector, d[i]->apic_id);
	return 0;
}

static void irq_shutdown_locked(struct irq_desc *d);

void irq_free_desc(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);
	uint64_t fl;

	might_sleep();
	if (!d)
		return;
	mutex_lock(&d->req_mutex);
	if (WARN(d->nact != 0, "irq %d: descriptor freed with %d handler(s)",
		 irq, d->nact)) {
		/* Free the handlers as free_irq would; callers that forgot
		 * them are buggy, but the vector must not leak. */
		d->nact = 0;
		irq_shutdown_locked(d);
	}
	__atomic_store_n(&d->in_use, 0, __ATOMIC_RELEASE);
	spin_lock_irqsave(&g_vector_lock, &fl);
	if (d->kind == IRQ_KIND_BLOCK && d->vector >= 0) {
		__atomic_store_n(&g_vector_irq[d->cpu][d->vector], 0,
				 __ATOMIC_RELEASE);
		irq_matrix_free(&g_matrix, (unsigned int)d->cpu,
				(unsigned int)d->vector, 0);
		d->vector = -1;
		d->cpu = -1;
	} else if (d->kind == IRQ_KIND_VECTOR) {
		if (d->managed)
			irq_matrix_remove_managed(&g_matrix, &d->affinity);
		else if (d->reserved)
			irq_matrix_remove_reserved(&g_matrix);
		d->reserved = 0;
	}
	spin_unlock_irqrestore(&g_vector_lock, fl);
	mutex_unlock(&d->req_mutex);
	desc_put(d);
}

/* ---- source operations (desc->lock held or descriptor not yet live) ---- */

static void desc_mask(struct irq_desc *d)
{
	if (d->masked)
		return;
	if (d->ops && d->ops->mask)
		d->ops->mask(d->irq, d->data);
	d->masked = 1;
}

static void desc_unmask(struct irq_desc *d)
{
	if (!d->masked)
		return;
	if (d->ops && d->ops->unmask)
		d->ops->unmask(d->irq, d->data);
	d->masked = 0;
}

static int desc_can_mask(const struct irq_desc *d)
{
	return d->ops && d->ops->mask && d->ops->unmask;
}

/* Send the vector to its CPU again (lost-while-disabled replay).  A
 * virtual source with a retrigger operation (a GPIO pad whose controller
 * cleared the event when it was taken) is asked to deliver it again. */
static void desc_retrigger(struct irq_desc *d)
{
	if (d->kind == IRQ_KIND_VIRTUAL && d->ops && d->ops->retrigger) {
		d->ops->retrigger(d->irq, d->data);
		return;
	}
	if (d->kind == IRQ_KIND_GSI || d->kind == IRQ_KIND_VIRTUAL ||
	    d->vector < 0 || d->cpu < 0)
		return;
	if (!lapic_is_available())
		return;
	lapic_send_ipi(d->apic_id, (uint32_t)d->vector);
}

/* A message-signalled source with a CPU vector of its own: disabling it
 * only marks it disabled (the source is masked by the edge flow if a
 * message actually comes meanwhile), and a message that came while it
 * was disabled is sent to its CPU again when it is enabled.  So nothing
 * depends on the device holding a message generated while its vector is
 * masked, and a message that had already reached the CPU is not lost.
 * An I/O APIC line (level, shared) and a child of a remapping domain (no
 * vector to send) are masked at the source when disabled. */
static int desc_lazy_disable(const struct irq_desc *d)
{
	return d->kind == IRQ_KIND_VECTOR || d->kind == IRQ_KIND_BLOCK;
}

/* The interrupt is enabled again (desc->lock held): what arrived while it
 * was disabled is replayed -- for a vector source always (its message
 * was taken by the CPU, the device does not send it again), for the
 * others when the source has no mask to hold it. */
static void desc_replay_pending(struct irq_desc *d)
{
	lockdep_assert_held(&d->lock);
	if (!d->pending)
		return;
	if (!desc_lazy_disable(d) && desc_can_mask(d) &&
	    !(d->kind == IRQ_KIND_VIRTUAL && d->ops && d->ops->retrigger))
		return;
	d->pending = 0;
	desc_retrigger(d);
}

/* ==========================================================================
 * Startup / shutdown (vector allocation per CPU)
 * ========================================================================== */

static int irq_gsi_trampoline(void *arg);

/* Give the descriptor a vector on a CPU of its mask and program the
 * source; the source is left masked.  req_mutex held. */
static int irq_startup_locked(struct irq_desc *d)
{
	cpumask_t targets, msk;
	unsigned int cpu = 0;
	uint64_t fl;
	int vec, rc;

	if (d->started)
		return 0;
	if (d->kind == IRQ_KIND_GSI) {
		d->masked = 0;
		desc_mask(d);
		rc = irq_request_gsi(d->gsi, irq_gsi_trampoline, d,
				     d->name ? d->name : "irq", d->gsi_active_low,
				     d->gsi_level);
		if (rc)
			return rc;
		/* The line's vector (assigned at creation, kept for good,
		 * unless a fixed-vector driver rerouted the line since). */
		if (WARN_ON_ONCE(irq_gsi_vector(d->gsi) != d->vector))
			d->vector = irq_gsi_vector(d->gsi);
		d->apic_id = irq_cpu_apic_id(0);
		d->started = 1;
		return 0;
	}
	if (d->kind == IRQ_KIND_BLOCK) {
		/* Vector fixed at creation; source already composed. */
		d->started = 1;
		if (d->ops && d->ops->startup)
			d->ops->startup(d->irq, d->data);
		return 0;
	}
	if (d->kind == IRQ_KIND_VIRTUAL) {
		spin_lock_irqsave(&d->lock, &fl);
		d->masked = 0;
		desc_mask(d);
		spin_unlock_irqrestore(&d->lock, fl);
		if (d->ops && d->ops->compose_msg)
			d->ops->compose_msg(d->irq, d->data, 0, 0);
		d->started = 1;
		if (d->ops && d->ops->startup)
			d->ops->startup(d->irq, d->data);
		return 0;
	}
	irq_target_cpus(&targets);
	spin_lock_irqsave(&g_vector_lock, &fl);
	if (d->managed) {
		vec = irq_matrix_alloc_managed(&g_matrix, &d->affinity, &cpu);
	} else {
		if (cpumask_and(&msk, &d->affinity, &targets))
			vec = irq_matrix_alloc(&g_matrix, &msk, d->reserved,
					       &cpu);
		else
			vec = -ENOSPC;
		if (vec < 0)
			vec = irq_matrix_alloc(&g_matrix, &targets,
					       d->reserved, &cpu);
		if (vec >= 0)
			d->reserved = 0;
	}
	if (vec >= 0) {
		BUG_ON(cpu >= MAX_CPUS);
		BUG_ON(vec < IRQ_MATRIX_FIRST || vec >= IRQ_MATRIX_END);
		BUG_ON(g_vector_irq[cpu][vec] != 0);
		d->cpu = (int)cpu;
		d->vector = vec;
		d->apic_id = irq_cpu_apic_id(cpu);
		__atomic_store_n(&g_vector_irq[cpu][vec],
				 (uint16_t)(d->irq - IRQ_NR_FIRST + 1),
				 __ATOMIC_RELEASE);
	}
	spin_unlock_irqrestore(&g_vector_lock, fl);
	if (vec < 0)
		return vec;
	/* Masked while the message is written; the mapping exists before
	 * the device can send to it. */
	spin_lock_irqsave(&d->lock, &fl);
	d->masked = 0;
	desc_mask(d);
	spin_unlock_irqrestore(&d->lock, fl);
	if (d->ops && d->ops->compose_msg)
		d->ops->compose_msg(d->irq, d->data, (uint8_t)d->vector,
				    d->apic_id);
	d->started = 1;
	if (d->ops && d->ops->startup)
		d->ops->startup(d->irq, d->data);
	return 0;
}

/* Mask, then release the vector.  req_mutex held; no handlers left. */
static void irq_shutdown_locked(struct irq_desc *d)
{
	uint64_t fl;

	if (!d->started)
		return;
	spin_lock_irqsave(&d->lock, &fl);
	desc_mask(d);
	d->pending = 0;
	spin_unlock_irqrestore(&d->lock, fl);
	if (d->ops && d->ops->shutdown)
		d->ops->shutdown(d->irq, d->data);

	if (d->kind == IRQ_KIND_VIRTUAL) {
		d->started = 0;
		return;
	}
	if (d->kind == IRQ_KIND_GSI) {
		irq_free(d->vector, irq_gsi_trampoline, d);
		/* Last user of the line gone: mask the pin as well, so a
		 * level line left asserted cannot storm. */
		if (irq_vector_nact(d->vector) == 0)
			ioapic_mask_gsi((uint8_t)d->gsi);
		d->started = 0;
		return;
	}
	if (d->kind == IRQ_KIND_BLOCK) {
		d->started = 0;
		return;
	}
	spin_lock_irqsave(&g_vector_lock, &fl);
	if (d->vector >= 0 && d->cpu >= 0) {
		__atomic_store_n(&g_vector_irq[d->cpu][d->vector], 0,
				 __ATOMIC_RELEASE);
		irq_matrix_free(&g_matrix, (unsigned int)d->cpu,
				(unsigned int)d->vector, d->managed);
	}
	if (!d->managed && !d->reserved) {
		irq_matrix_reserve(&g_matrix);
		d->reserved = 1;
	}
	d->vector = -1;
	d->cpu = -1;
	d->started = 0;
	spin_unlock_irqrestore(&g_vector_lock, fl);
}

/* ==========================================================================
 * Interrupt threads
 * ========================================================================== */

struct irq_thread {
	task_t *task;
	void *stack;
	struct wait_queue_head wq;
	struct irq_desc *desc; /* NULL while in the pool */
	struct irq_naction *action;
	volatile int wake; /* the primary handler asked for a run */
	volatile int busy; /* thread_fn running */
	struct irq_thread *pool_next;
};

static spinlock_t g_thread_pool_lock = SPINLOCK_INIT("irq_threads");
static struct irq_thread *g_thread_pool;
static unsigned int g_nthreads;

/* Lift the oneshot mask when no thread holds it any more and nobody
 * disabled the interrupt meanwhile.  desc->lock held. */
static void desc_oneshot_unmask_locked(struct irq_desc *d)
{
	lockdep_assert_held(&d->lock);
	if (d->threads_oneshot || d->depth != 0 || !d->masked || !d->nact)
		return;
	desc_unmask(d);
	desc_replay_pending(d);
}

/* The thread function of a oneshot action returned: drop its hold on the
 * mask.  The hard handler that woke the thread sets that hold (and masks)
 * only after its handlers ran, under desc->lock, so while a hard handler
 * is still running on another CPU the hold may not be there yet: wait for
 * it, or the hold set afterwards would never be dropped.  A thread woken
 * again meanwhile keeps the mask for its next run. */
static void irq_finalize_oneshot(struct irq_thread *t, struct irq_desc *d,
				 unsigned long flags, unsigned int bit)
{
	uint64_t fl;

	if (!(flags & IRQF_ONESHOT))
		return;
	for (;;) {
		spin_lock_irqsave(&d->lock, &fl);
		if (!d->running)
			break;
		spin_unlock_irqrestore(&d->lock, fl);
		__asm__ volatile("pause" ::: "memory");
	}
	if (!__atomic_load_n(&t->wake, __ATOMIC_SEQ_CST)) {
		d->threads_oneshot &= ~(1u << bit);
		desc_oneshot_unmask_locked(d);
	}
	spin_unlock_irqrestore(&d->lock, fl);
}

static void irq_thread_main(void *arg)
{
	struct irq_thread *t = arg;

	for (;;) {
		struct irq_desc *d;
		struct irq_naction *a;
		irq_handler_t fn = NULL;
		void *fn_arg = NULL;
		unsigned long flags = 0;
		unsigned int bit = 0;
		uint64_t fl;

		wait_event(t->wq, __atomic_load_n(&t->wake, __ATOMIC_ACQUIRE));
		/* busy before wake is cleared: free_irq() tests wake first,
		 * then busy, and so never sees the thread idle while a run
		 * is still to come. */
		__atomic_store_n(&t->busy, 1, __ATOMIC_SEQ_CST);
		__atomic_store_n(&t->wake, 0, __ATOMIC_SEQ_CST);
		d = __atomic_load_n(&t->desc, __ATOMIC_ACQUIRE);
		if (d) {
			/* The action entry moves when another handler of
			 * the descriptor is freed: copy it under the lock. */
			spin_lock_irqsave(&d->lock, &fl);
			a = __atomic_load_n(&t->action, __ATOMIC_ACQUIRE);
			if (a) {
				fn = a->thread_fn;
				fn_arg = a->arg;
				flags = a->flags;
				bit = a->thread_bit;
			}
			spin_unlock_irqrestore(&d->lock, fl);
			if (fn) {
				if (fn(fn_arg) == IRQ_HANDLED)
					__atomic_add_fetch(&d->threads_handled,
							   1, __ATOMIC_RELAXED);
				irq_finalize_oneshot(t, d, flags, bit);
			}
		}
		__atomic_store_n(&t->busy, 0, __ATOMIC_SEQ_CST);
		if (d) {
			__atomic_sub_fetch(&d->threads_active, 1,
					   __ATOMIC_SEQ_CST);
			wake_up(&d->wait_for_threads);
		}
	}
}

/* A thread from the pool, or a new one.  May sleep. */
static struct irq_thread *irq_thread_get(const char *name, int irq)
{
	struct irq_thread *t;
	uint64_t fl;

	spin_lock_irqsave(&g_thread_pool_lock, &fl);
	t = g_thread_pool;
	if (t)
		g_thread_pool = t->pool_next;
	spin_unlock_irqrestore(&g_thread_pool_lock, fl);
	if (!t) {
		if (sched_current() == NULL)
			return NULL;
		t = kcalloc(1, sizeof(*t));
		if (!t)
			return NULL;
		wq_head_init(&t->wq, "irq_thread");
		t->stack = mm_alloc_guarded_kstack(IRQ_THREAD_STACK);
		if (!t->stack) {
			kfree(t);
			return NULL;
		}
		t->task = sched_add_task(irq_thread_main, t, t->stack,
					 IRQ_THREAD_STACK);
		if (!t->task) {
			/* The guarded stack stays allocated: the task may
			 * have been half set up; a few KiB lost once. */
			kfree(t);
			return NULL;
		}
		__atomic_add_fetch(&g_nthreads, 1, __ATOMIC_RELAXED);
	}
	t->pool_next = NULL;
	if (t->task)
		ksnprintf(t->task->comm, sizeof(t->task->comm), "irq/%d-%s",
			  irq, name ? name : "irq");
	return t;
}

/* Back to the pool (idle, detached). */
static void irq_thread_put(struct irq_thread *t)
{
	uint64_t fl;

	__atomic_store_n(&t->action, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&t->desc, NULL, __ATOMIC_RELEASE);
	if (t->task)
		ksnprintf(t->task->comm, sizeof(t->task->comm), "irq/idle");
	spin_lock_irqsave(&g_thread_pool_lock, &fl);
	t->pool_next = g_thread_pool;
	g_thread_pool = t;
	spin_unlock_irqrestore(&g_thread_pool_lock, fl);
}

/* ==========================================================================
 * Flow handlers
 * ========================================================================== */

/* Is the descriptor's "nobody cared" accounting meaningful?  Only where
 * the descriptor sees every handler of its source.  Not for a child of a
 * demultiplexing parent (VMD): the parent cannot tell which child fired
 * and runs them all, so a quiet child sees the busy ones' interrupts as
 * unclaimed -- such children are untracked.  Not for an INTx descriptor
 * either: the I/O APIC line is shared with the vector-indexed handlers and
 * the fixed dispatch arms, whose results it does not see, so it cannot
 * know whether anybody on the LINE claimed an interrupt it did not. */
static int desc_tracked(const struct irq_desc *d)
{
	return d->kind == IRQ_KIND_VECTOR || d->kind == IRQ_KIND_BLOCK;
}

static void report_bad_irq(struct irq_desc *d, int action_ret)
{
	static int budget = 100;

	lockdep_assert_held(&d->lock);
	if (budget <= 0)
		return;
	budget--;
	if (action_ret > (IRQ_HANDLED | IRQ_WAKE_THREAD))
		kprintf("irq event %d: bogus return value %x\n", d->irq,
			action_ret);
	else
		kprintf("irq %d: nobody cared\n", d->irq);
	kprintf("handlers:\n");
	for (int i = 0; i < d->nact; i++)
		kprintf("  [<%p>] %s%s\n", (void *)d->act[i].handler,
			d->act[i].name ? d->act[i].name : "?",
			d->act[i].thread_fn ? " threaded" : "");
}

/* Unhandled-interrupt accounting of one hard interrupt (the rules are in
 * irq_spurious_note()): a source claimed by nobody 99,900 times out of
 * 100,000 is disabled.  `action_ret' is the OR of what every handler
 * returned.  desc->lock held. */
static void note_interrupt(struct irq_desc *d, int action_ret)
{
	lockdep_assert_held(&d->lock);
	switch (irq_spurious_note(&d->spur, action_ret,
				  __atomic_load_n(&d->threads_handled,
						  __ATOMIC_RELAXED),
				  hrtimer_now_ns)) {
	case IRQ_SPURIOUS_BOGUS:
		report_bad_irq(d, action_ret);
		break;
	case IRQ_SPURIOUS_STUCK:
		report_bad_irq(d, IRQ_NONE);
		kprintf("Disabling IRQ #%d\n", d->irq);
		d->spurious_disabled = 1;
		d->depth++;
		desc_mask(d);
		break;
	default:
		break;
	}
}

/* Run the actions once, with desc->lock dropped.  Returns the OR of the
 * handlers' results (IRQ_NONE: nobody claimed the interrupt). */
static int handle_irq_event(struct irq_desc *d, uint64_t *fl)
{
	struct irq_naction acts[IRQ_DESC_MAX_ACTIONS];
	int n = d->nact, action_ret = IRQ_NONE;
	uint32_t want_oneshot = 0;

	__builtin_memcpy(acts, d->act, sizeof(struct irq_naction) * (size_t)n);
	d->running = 1;
	spin_unlock_irqrestore(&d->lock, *fl);

	for (int i = 0; i < n; i++) {
		struct irq_naction *a = &acts[i];
		int r = a->handler ? a->handler(a->arg) : IRQ_WAKE_THREAD;

		if (r == IRQ_WAKE_THREAD) {
			struct irq_thread *t = a->thread;

			if (WARN_RATELIMIT(!t, "irq %d: IRQ_WAKE_THREAD without a thread",
					   d->irq)) {
				action_ret |= IRQ_HANDLED;
				continue;
			}
			if (a->flags & IRQF_ONESHOT)
				want_oneshot |= 1u << a->thread_bit;
			if (!__atomic_exchange_n(&t->wake, 1,
						 __ATOMIC_SEQ_CST))
				__atomic_add_fetch(&d->threads_active, 1,
						   __ATOMIC_SEQ_CST);
			wake_up(&t->wq);
		}
		action_ret |= r;
	}

	spin_lock_irqsave(&d->lock, fl);
	if (want_oneshot) {
		/* Keep the source masked until the thread(s) finish
		 * (irq_finalize_oneshot waits for `running' to clear, so
		 * this hold is in place before any thread drops it). */
		d->threads_oneshot |= want_oneshot;
		desc_mask(d);
	}
	d->running = 0;
	if (desc_tracked(d))
		note_interrupt(d, action_ret);
	return action_ret;
}

/* The edge flow: one message, one run of the handlers.  `ack': this
 * CPU's local APIC is acknowledged here (not for a virtual descriptor,
 * whose parent did it). */
static void irq_handle_edge(struct irq_desc *d, int ack)
{
	uint64_t fl;

	spin_lock_irqsave(&d->lock, &fl);
	__atomic_add_fetch(&d->count, 1, __ATOMIC_RELAXED);
	/* Acknowledge first: a new edge can be latched while the handlers
	 * run (it then comes back through here). */
	if (ack)
		lapic_eoi();
	if (d->depth > 0 || d->nact == 0 || d->threads_oneshot) {
		/* Disabled (or the thread still owns it): remember it and
		 * make sure the source is quiet. */
		d->pending = 1;
		if (desc_can_mask(d))
			desc_mask(d);
		spin_unlock_irqrestore(&d->lock, fl);
		return;
	}
	if (d->running) {
		/* Running on another CPU (an affinity move): that CPU runs
		 * the handlers once more. */
		d->pending = 1;
		spin_unlock_irqrestore(&d->lock, fl);
		return;
	}
	do {
		d->pending = 0;
		(void)handle_irq_event(d, &fl);
	} while (d->pending && d->depth == 0 && d->nact &&
		 !d->threads_oneshot);
	spin_unlock_irqrestore(&d->lock, fl);
}

void irq_handle_virtual(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	if (!d || WARN_RATELIMIT(d->kind != IRQ_KIND_VIRTUAL,
				 "irq %d: not a virtual interrupt", irq))
		return;
	irq_handle_edge(d, 0);
}

/* The level flow for an INTx descriptor, called by the legacy
 * dispatcher, which acknowledges when it returns 1. */
static int irq_gsi_trampoline(void *arg)
{
	struct irq_desc *d = arg;
	uint64_t fl;
	int handled;

	if (!d || !__atomic_load_n(&d->in_use, __ATOMIC_ACQUIRE))
		return 0;
	spin_lock_irqsave(&d->lock, &fl);
	if (d->depth > 0 || d->nact == 0 || d->running ||
	    d->threads_oneshot) {
		/* Masked at the device -- by disable_irq, or held masked by
		 * a oneshot thread that has not finished: not ours to claim
		 * (the mask stops our device from asserting; another device
		 * on the shared line is what fired).  Its handlers run
		 * again only once the line is unmasked, as with a level
		 * line that stays masked until the thread is done. */
		spin_unlock_irqrestore(&d->lock, fl);
		return 0;
	}
	handled = handle_irq_event(d, &fl) != IRQ_NONE;
	if (handled)
		__atomic_add_fetch(&d->count, 1, __ATOMIC_RELAXED);
	spin_unlock_irqrestore(&d->lock, fl);
	return handled;
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

static uint64_t g_spurious_vectors;

int irq_dispatch(uint64_t vector)
{
	if (vector > 255)
		return 0;
	if (vector >= IRQ_MATRIX_FIRST && vector < IRQ_MATRIX_END &&
	    __atomic_load_n(&g_matrix_ready, __ATOMIC_ACQUIRE)) {
		uint32_t cpu = this_cpu_id();
		uint16_t idx = 0;

		if (cpu < MAX_CPUS)
			idx = __atomic_load_n(&g_vector_irq[cpu][vector],
					      __ATOMIC_ACQUIRE);
		if (idx) {
			struct irq_desc *d = __atomic_load_n(
				&g_descs[idx - 1], __ATOMIC_ACQUIRE);

			if (d) {
				irq_handle_edge(d, 1);
				return 1;
			}
		}
		if (!irq_matrix_is_system(&g_matrix, (unsigned int)vector) &&
		    irq_vector_nact((int)vector) == 0) {
			/* A per-CPU vector nobody owns any more (a message
			 * in flight at shutdown): acknowledge and drop.
			 * System vectors (the pool, I/O APIC lines) go on
			 * to the vector-indexed table and the fixed arms. */
			__atomic_add_fetch(&g_spurious_vectors, 1,
					   __ATOMIC_RELAXED);
			WARN_RATELIMIT(1, "irq: no handler for vector %lu on CPU %u",
				       (unsigned long)vector, cpu);
			lapic_eoi();
			return 1;
		}
	}
	return irq_dispatch_legacy(vector);
}

/* ==========================================================================
 * request_irq / free_irq
 * ========================================================================== */

/* Must a thread-only handler keep the source masked until it ran. */
static int desc_needs_oneshot(const struct irq_desc *d)
{
	return d->kind == IRQ_KIND_GSI ||
	       (d->ops && (d->ops->flags & IRQ_CHIP_ONESHOT_REQUIRED));
}

static int irq_default_primary(void *arg)
{
	(void)arg;
	return IRQ_WAKE_THREAD;
}

int request_irq(int irq, irq_handler_t fn, unsigned long flags,
		const char *name, void *arg)
{
	return request_threaded_irq(irq, fn, NULL, flags, name, arg);
}

int request_threaded_irq(int irq, irq_handler_t fn, irq_handler_t thread_fn,
			 unsigned long flags, const char *name, void *arg)
{
	struct irq_desc *d = irq_to_desc(irq);
	struct irq_thread *t = NULL;
	struct irq_naction *a;
	uint64_t fl;
	int rc = 0, first;
	unsigned int bit = 0;

	might_sleep();
	if (!d)
		return -EINVAL;
	if (!fn && !thread_fn)
		return -EINVAL;
	if ((flags & IRQF_SHARED) && !arg)
		return -EINVAL;
	if (!fn) {
		fn = irq_default_primary;
		/* A level line must stay masked until the thread ran;
		 * message-signalled sources are edge and need not be.  A
		 * source behind a demultiplexing controller (a GPIO pad)
		 * says so with IRQ_CHIP_ONESHOT_REQUIRED. */
		if (desc_needs_oneshot(d) && !(flags & IRQF_ONESHOT))
			return -EINVAL;
	}
	if (!desc_needs_oneshot(d) && fn == irq_default_primary)
		flags &= ~(unsigned long)IRQF_ONESHOT;

	mutex_lock(&d->req_mutex);
	if (d->nact >= IRQ_DESC_MAX_ACTIONS) {
		rc = -EBUSY;
		goto out;
	}
	if (d->nact) {
		/* Sharing: everybody must agree to it, and on ONESHOT. */
		if (!(flags & IRQF_SHARED) || !(d->act[0].flags & IRQF_SHARED) ||
		    ((flags ^ d->act[0].flags) & IRQF_ONESHOT)) {
			rc = -EBUSY;
			goto out;
		}
	}
	if (thread_fn) {
		uint32_t used = 0;

		for (int i = 0; i < d->nact; i++)
			if (d->act[i].thread)
				used |= 1u << d->act[i].thread_bit;
		while (bit < IRQ_DESC_MAX_ACTIONS && (used & (1u << bit)))
			bit++;
		BUG_ON(bit >= IRQ_DESC_MAX_ACTIONS);
		t = irq_thread_get(name, irq);
		if (!t) {
			rc = -ENOMEM;
			goto out;
		}
	}
	first = d->nact == 0;
	if (first) {
		d->name = name;
		rc = irq_startup_locked(d);
		if (rc) {
			if (t)
				irq_thread_put(t);
			goto out;
		}
	}

	spin_lock_irqsave(&d->lock, &fl);
	a = &d->act[d->nact];
	a->handler = fn;
	a->thread_fn = thread_fn;
	a->arg = arg;
	a->name = name;
	a->flags = flags;
	a->thread = t;
	a->thread_bit = bit;
	if (t) {
		__atomic_store_n(&t->action, a, __ATOMIC_RELEASE);
		__atomic_store_n(&t->desc, d, __ATOMIC_RELEASE);
	}
	d->nact++;
	/* A new handler: the unhandled-interrupt detection starts over, and
	 * a shared source it disabled gets another chance. */
	d->spur.irq_count = 0;
	d->spur.unhandled = 0;
	if (!first && d->spurious_disabled) {
		d->spurious_disabled = 0;
		if (!WARN_ON_ONCE(d->depth == 0) && --d->depth == 0 &&
		    !d->threads_oneshot) {
			desc_unmask(d);
			desc_replay_pending(d);
		}
	}
	if (first) {
		d->spurious_disabled = 0;
		d->spur.threads_handled_last = 0;
		if (flags & IRQF_NO_AUTOEN) {
			d->depth = 1;
		} else {
			d->depth = 0;
			desc_unmask(d);
			desc_replay_pending(d);
		}
	}
	spin_unlock_irqrestore(&d->lock, fl);
out:
	mutex_unlock(&d->req_mutex);
	return rc;
}

static void wait_desc_idle(struct irq_desc *d)
{
	uint64_t fl;

	for (;;) {
		int busy;

		spin_lock_irqsave(&d->lock, &fl);
		busy = d->running;
		spin_unlock_irqrestore(&d->lock, fl);
		if (!busy)
			break;
		__asm__ volatile("pause");
	}
}

int synchronize_hardirq(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	if (!d)
		return true;
	wait_desc_idle(d);
	return __atomic_load_n(&d->threads_active, __ATOMIC_ACQUIRE) == 0;
}

void synchronize_irq(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	might_sleep();
	if (!d)
		return;
	wait_desc_idle(d);
	if (sched_current() != NULL && !irqs_disabled())
		wait_event(d->wait_for_threads,
			   __atomic_load_n(&d->threads_active,
					   __ATOMIC_ACQUIRE) == 0);
}

const char *free_irq(int irq, void *arg)
{
	struct irq_desc *d = irq_to_desc(irq);
	struct irq_naction removed;
	const char *name = NULL;
	uint64_t fl;
	int i, found = -1;

	might_sleep();
	if (!d)
		return NULL;
	mutex_lock(&d->req_mutex);
	spin_lock_irqsave(&d->lock, &fl);
	for (i = 0; i < d->nact; i++) {
		if (d->act[i].arg == arg) {
			found = i;
			break;
		}
	}
	if (found < 0) {
		spin_unlock_irqrestore(&d->lock, fl);
		mutex_unlock(&d->req_mutex);
		WARN(1, "irq %d: free_irq of a handler that is not there", irq);
		return NULL;
	}
	removed = d->act[found];
	for (i = found + 1; i < d->nact; i++) {
		d->act[i - 1] = d->act[i];
		if (d->act[i - 1].thread)
			__atomic_store_n(&d->act[i - 1].thread->action,
					 &d->act[i - 1], __ATOMIC_RELEASE);
	}
	d->nact--;
	__builtin_memset(&d->act[d->nact], 0, sizeof(d->act[0]));
	if (d->nact == 0)
		desc_mask(d);
	spin_unlock_irqrestore(&d->lock, fl);
	name = removed.name;

	/* No CPU runs the removed handler after this. */
	wait_desc_idle(d);
	if (removed.thread) {
		struct irq_thread *t = removed.thread;

		/* Detach first so a wake that races here runs nothing. */
		__atomic_store_n(&t->action, NULL, __ATOMIC_RELEASE);
		while (__atomic_load_n(&t->wake, __ATOMIC_SEQ_CST) ||
		       __atomic_load_n(&t->busy, __ATOMIC_SEQ_CST)) {
			if (sched_current() != NULL && !irqs_disabled())
				sched_yield_in_kernel();
			else
				__asm__ volatile("pause");
		}
		spin_lock_irqsave(&d->lock, &fl);
		/* The detached thread will not finalize: drop its hold on
		 * the oneshot mask here, or the remaining sharers stay
		 * masked. */
		d->threads_oneshot &= ~(1u << removed.thread_bit);
		desc_oneshot_unmask_locked(d);
		spin_unlock_irqrestore(&d->lock, fl);
		irq_thread_put(t);
	}
	if (d->nact == 0) {
		irq_shutdown_locked(d);
		spin_lock_irqsave(&d->lock, &fl);
		d->depth = 0;
		d->threads_oneshot = 0;
		d->pending = 0;
		spin_unlock_irqrestore(&d->lock, fl);
		if (sched_current() != NULL && !irqs_disabled())
			wait_event(d->wait_for_threads,
				   __atomic_load_n(&d->threads_active,
						   __ATOMIC_ACQUIRE) == 0);
	}
	mutex_unlock(&d->req_mutex);
	return name;
}

/* ==========================================================================
 * disable / enable / affinity / queries
 * ========================================================================== */

void disable_irq_nosync(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);
	uint64_t fl;

	if (!d)
		return;
	spin_lock_irqsave(&d->lock, &fl);
	/* Lazily for a vector source: the edge flow masks it when a
	 * message comes while it is disabled (desc_lazy_disable). */
	if (d->depth++ == 0 && !desc_lazy_disable(d))
		desc_mask(d);
	spin_unlock_irqrestore(&d->lock, fl);
}

void disable_irq(int irq)
{
	might_sleep();
	disable_irq_nosync(irq);
	synchronize_irq(irq);
}

void enable_irq(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);
	uint64_t fl;

	if (!d)
		return;
	spin_lock_irqsave(&d->lock, &fl);
	if (WARN(d->depth == 0, "irq %d: unbalanced enable_irq", irq)) {
		spin_unlock_irqrestore(&d->lock, fl);
		return;
	}
	if (--d->depth == 0 && d->nact && !d->threads_oneshot) {
		desc_unmask(d);
		/* A message that came while disabled was taken by its CPU
		 * and only marked pending there: send it again. */
		desc_replay_pending(d);
	}
	spin_unlock_irqrestore(&d->lock, fl);
}

const cpumask_t *irq_get_affinity_mask(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	return d ? &d->affinity : NULL;
}

int irq_get_effective_cpu(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	if (!d)
		return -EINVAL;
	return d->cpu;
}

int irq_is_managed(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	return d ? d->managed : 0;
}

uint64_t irq_get_count(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	return d ? __atomic_load_n(&d->count, __ATOMIC_RELAXED) : 0;
}

static void irq_sleep_ms(unsigned int ms)
{
	if (sched_current() != NULL && !irqs_disabled()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0)
			;
	} else {
		lapic_delay_ms(ms);
	}
}

int irq_set_affinity(int irq, const cpumask_t *mask)
{
	struct irq_desc *d = irq_to_desc(irq);
	cpumask_t targets, msk;
	unsigned int cpu = 0;
	int old_cpu, old_vec, vec;
	uint64_t fl;

	might_sleep();
	if (!d || !mask)
		return -EINVAL;
	if (d->managed || d->no_move)
		return -EINVAL;
	irq_target_cpus(&targets);
	if (!cpumask_and(&msk, mask, &targets))
		return -EINVAL;

	mutex_lock(&d->req_mutex);
	if (!d->started) {
		cpumask_copy(&d->affinity, &msk);
		mutex_unlock(&d->req_mutex);
		return 0;
	}
	if (d->cpu >= 0 && cpumask_test_cpu((unsigned int)d->cpu, &msk)) {
		/* The current target is acceptable: nothing to move. */
		cpumask_copy(&d->affinity, &msk);
		mutex_unlock(&d->req_mutex);
		return 0;
	}
	spin_lock_irqsave(&g_vector_lock, &fl);
	vec = irq_matrix_alloc(&g_matrix, &msk, 0, &cpu);
	if (vec >= 0) {
		BUG_ON(g_vector_irq[cpu][vec] != 0);
		__atomic_store_n(&g_vector_irq[cpu][vec],
				 (uint16_t)(d->irq - IRQ_NR_FIRST + 1),
				 __ATOMIC_RELEASE);
	}
	spin_unlock_irqrestore(&g_vector_lock, fl);
	if (vec < 0) {
		mutex_unlock(&d->req_mutex);
		return vec;
	}
	old_cpu = d->cpu;
	old_vec = d->vector;
	/* Reprogram: both the old and the new (cpu, vector) map to the
	 * descriptor while messages may still be on their way. */
	spin_lock_irqsave(&d->lock, &fl);
	d->cpu = (int)cpu;
	d->vector = vec;
	d->apic_id = irq_cpu_apic_id(cpu);
	cpumask_copy(&d->affinity, &msk);
	spin_unlock_irqrestore(&d->lock, fl);
	if (d->ops && d->ops->compose_msg)
		d->ops->compose_msg(d->irq, d->data, (uint8_t)vec, d->apic_id);
	/* A source without a mask bit may have fired into the window
	 * between the halves of the message: one replay on the new
	 * vector makes sure nothing is lost (handlers tolerate a call
	 * with nothing to do). */
	if (!desc_can_mask(d)) {
		spin_lock_irqsave(&d->lock, &fl);
		if (d->depth == 0 && !d->masked)
			desc_retrigger(d);
		spin_unlock_irqrestore(&d->lock, fl);
	}
	irq_sleep_ms(IRQ_MOVE_CLEANUP_MS);
	spin_lock_irqsave(&g_vector_lock, &fl);
	if (old_vec >= 0 && old_cpu >= 0) {
		__atomic_store_n(&g_vector_irq[old_cpu][old_vec], 0,
				 __ATOMIC_RELEASE);
		irq_matrix_free(&g_matrix, (unsigned int)old_cpu,
				(unsigned int)old_vec, 0);
	}
	spin_unlock_irqrestore(&g_vector_lock, fl);
	mutex_unlock(&d->req_mutex);
	return 0;
}

/* ---- PCI-core helpers --------------------------------------------------- */

int irq_desc_target(int irq, unsigned int *cpu, unsigned int *vector,
		    uint32_t *apic_id)
{
	struct irq_desc *d = irq_to_desc(irq);

	if (!d)
		return -EINVAL;
	if (d->kind == IRQ_KIND_VIRTUAL) {
		if (!d->started)
			return -ENOENT;
		if (cpu)
			*cpu = 0;
		if (vector)
			*vector = 0;
		if (apic_id)
			*apic_id = 0;
		return 0;
	}
	if (d->vector < 0 || d->cpu < 0 ||
	    (!d->started && d->kind != IRQ_KIND_BLOCK))
		return -ENOENT;
	if (cpu)
		*cpu = (unsigned int)d->cpu;
	if (vector)
		*vector = (unsigned int)d->vector;
	if (apic_id)
		*apic_id = d->apic_id;
	return 0;
}

void irq_desc_recompose(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	if (!d || d->kind == IRQ_KIND_GSI)
		return;
	if (d->kind == IRQ_KIND_VIRTUAL) {
		if (d->started && d->ops && d->ops->compose_msg)
			d->ops->compose_msg(d->irq, d->data, 0, 0);
		return;
	}
	if (d->vector < 0 || d->cpu < 0)
		return;
	if (!d->started && d->kind != IRQ_KIND_BLOCK)
		return;
	if (d->ops && d->ops->compose_msg)
		d->ops->compose_msg(d->irq, d->data, (uint8_t)d->vector,
				    d->apic_id);
}

int irq_desc_masked(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	return d ? d->masked : 1;
}

int irq_desc_retrigger(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);
	uint64_t fl;

	if (!d)
		return -EINVAL;
	spin_lock_irqsave(&d->lock, &fl);
	desc_retrigger(d);
	spin_unlock_irqrestore(&d->lock, fl);
	return 0;
}

/* ==========================================================================
 * Trigger type and wake-up
 * ========================================================================== */

int irq_set_irq_type(int irq, unsigned int type)
{
	struct irq_desc *d = irq_to_desc(irq);
	uint64_t fl;
	int rc;

	if (!d || (type & ~IRQ_TYPE_SENSE_MASK))
		return -EINVAL;
	/* Level high and low at once, or a level and an edge, is no trigger
	 * a pin can have. */
	if ((type & IRQ_TYPE_LEVEL_MASK) == IRQ_TYPE_LEVEL_MASK ||
	    ((type & IRQ_TYPE_LEVEL_MASK) && (type & IRQ_TYPE_EDGE_BOTH)))
		return -EINVAL;
	if (!d->ops || !d->ops->set_type)
		return -ENOSYS;
	spin_lock_irqsave(&d->lock, &fl);
	rc = d->ops->set_type(d->irq, d->data, type);
	if (rc == 0)
		d->trigger = type;
	spin_unlock_irqrestore(&d->lock, fl);
	return rc;
}

unsigned int irq_get_trigger_type(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	if (!d)
		return IRQ_TYPE_NONE;
	if (d->kind == IRQ_KIND_GSI && d->trigger == IRQ_TYPE_NONE) {
		if (d->gsi_level)
			return d->gsi_active_low ? IRQ_TYPE_LEVEL_LOW :
						   IRQ_TYPE_LEVEL_HIGH;
		return d->gsi_active_low ? IRQ_TYPE_EDGE_FALLING :
					   IRQ_TYPE_EDGE_RISING;
	}
	return d->trigger;
}

int irq_set_irq_wake(int irq, unsigned int on)
{
	struct irq_desc *d = irq_to_desc(irq);
	uint64_t fl;
	int rc = 0;

	if (!d)
		return -EINVAL;
	spin_lock_irqsave(&d->lock, &fl);
	if (on) {
		if (d->wake_depth == 0 && d->ops && d->ops->set_wake)
			rc = d->ops->set_wake(d->irq, d->data, 1);
		if (rc == 0)
			d->wake_depth++;
	} else if (WARN(d->wake_depth == 0,
			"irq %d: unbalanced disable_irq_wake", irq)) {
		rc = -EINVAL;
	} else {
		if (d->wake_depth == 1 && d->ops && d->ops->set_wake)
			rc = d->ops->set_wake(d->irq, d->data, 0);
		/* The count drops even if the source refused: the caller
		 * has given the wake-up up either way. */
		d->wake_depth--;
	}
	spin_unlock_irqrestore(&d->lock, fl);
	return rc;
}

int irq_is_wake_enabled(int irq)
{
	struct irq_desc *d = irq_to_desc(irq);

	return d ? d->wake_depth != 0 : 0;
}
