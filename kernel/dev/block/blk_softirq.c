// LikeOS -- the block softirq: deferred completion of storage requests
//
// A disk controller's interrupt handler does the least it can with
// interrupts off: it acknowledges the controller, collects the commands
// that finished and does the transport bookkeeping for them.  The rest --
// ending the block request, copying a bounce buffer back, waking the task
// that waits, accounting, restarting the queue -- is handed to the block
// softirq through blk_softirq_complete().  This file is that softirq.
//
// The pieces, per CPU:
//
//   * a done list.  Entries are pushed onto it lock-free from any CPU
//     (compare-and-swap on the head) and taken off whole, with one atomic
//     exchange, by the softirq of the CPU that owns it.  Taken off, the
//     list is in reverse push order; it is turned around so entries finish
//     in the order they completed;
//
//   * a pending flag: "this CPU has block softirq work".  Whoever turns a
//     done list from empty to non-empty raises it, on the owning CPU --
//     directly when it is that CPU, by an inter-processor interrupt
//     (IPI_BLOCK_SOFTIRQ_VECTOR) otherwise.  A push onto a list that was
//     already non-empty raises nothing and sends nothing: whoever made it
//     non-empty did, and the softirq that empties it sees the new entry
//     too.  So a burst of completions aimed at one CPU costs one interrupt;
//
//   * a run of the softirq.  It happens at the tail of a hard interrupt
//     (blk_softirq_irq_tail(), called by every interrupt and IPI dispatch
//     once the handler and its acknowledgement are done), when that
//     interrupt is the outermost one and did not interrupt a run already
//     in progress.  Interrupts are enabled while the entries run; a hard
//     interrupt taken meanwhile neither starts a second run (the CPU is
//     marked as running the softirq) nor switches tasks on the way out
//     (irqentry_exit asks blk_in_softirq() first): the run sits on the
//     interrupted task's stack and must be finished on it, by this CPU;
//
//   * bounds.  One run restarts while new work keeps arriving, but at
//     most BLK_SOFTIRQ_MAX_RESTART times, only within
//     BLK_SOFTIRQ_MAX_TIME_MS of the clock tick, and never when the
//     interrupted task is due to be preempted.  Whatever is left then goes
//     to kblksoftirqd/N, a thread pinned to the CPU, which runs it as an
//     ordinary schedulable task;
//
//   * raising outside an interrupt.  A completion requested from process
//     context (not inside a hard interrupt, not inside a softirq run)
//     queues the entry and wakes kblksoftirqd/N -- there is no interrupt
//     tail coming to run it.
//
// Where an entry runs.  blk_softirq_complete() takes the CPU that
// submitted the request.  When that is this CPU, or a CPU sharing this
// CPU's last-level cache, the completion is finished here: the data is in
// a cache both can reach and an interrupt would cost more than the miss.
// Otherwise the entry goes onto the submitter's done list and an IPI runs
// the softirq there, so the waiting task is woken -- and the request's
// memory touched -- by the CPU it is running on.
//
// The cache topology comes from the processor's deterministic cache
// parameters: CPUID leaf 4 on Intel-compatible processors (the last level
// reported, L3 or else L2), leaf 0x8000001D on AMD processors with
// topology extensions, the per-family rules of AMD and Hygon processors
// that have none, and the package when nothing finer is reported.  The
// number of logical processors sharing the cache gives the low APIC ID
// bits that differ inside one cache domain; two CPUs share the cache when
// their APIC IDs agree above them.  Each kblksoftirqd/N repeats the
// detection on its own CPU when it starts, so a processor whose cores
// report different cache parameters is described per core.  A virtual
// machine reports what its virtual topology says: VMware gives each
// virtual socket its own last-level cache, so with the usual one socket
// of N cores every vCPU shares the cache with every other (all
// completions finish on the interrupted CPU), and with N sockets of one
// core each none share (a completion for a request submitted on another
// vCPU is carried there by an IPI).  When the processor reports nothing
// usable every CPU is treated as sharing, which is the cheap answer: no
// IPIs.
//
// This framework is the storage stack's own.  It shares nothing with the
// networking softirq.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/dev/block/blk_softirq.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/irq.h>
#include <kernel/hal/lapic.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* A run restarts at most this many times when new work arrived while it
 * ran: the bound that holds even when the clock does not advance (the
 * tick itself may be what is held off). */
#define BLK_SOFTIRQ_MAX_RESTART 10
/* ... and only while the clock tick has not moved past this budget since
 * the run began, rounded up to whole ticks (at least one). */
#define BLK_SOFTIRQ_MAX_TIME_MS 2

#define KBLKSOFTIRQD_STACK_SIZE (16u * 1024u)

#define RFLAGS_IF (1ULL << 9)

/* The per-CPU state.  One cache line apart so CPUs pushing onto their own
 * lists do not share lines. */
struct blk_sirq_cpu {
	/* Entries waiting for this CPU's softirq, newest first.  Pushed
	 * from any CPU, emptied only by this CPU. */
	struct blk_softirq_entry *volatile done;
	/* The block softirq is raised on this CPU. */
	volatile uint32_t pending;
	/* This CPU is running the block softirq (at an interrupt tail, in
	 * kblksoftirqd or in a poller).  Read by this CPU's own interrupt
	 * paths only. */
	volatile int active;
	/* Depth of hard interrupt handling on this CPU. */
	volatile int hardirq;
	/* kblksoftirqd/N, once started. */
	task_t *volatile thread;
	/* Last-level cache identifier: valid once computed. */
	volatile int llc_valid;
	volatile uint32_t llc_id;
	/* Statistics. */
	uint64_t runs; /* runs started */
	uint64_t restarts; /* restarts within a run */
	uint64_t handovers; /* runs that left work to kblksoftirqd */
	uint64_t entries; /* entries finished on this CPU */
	uint64_t thread_runs; /* runs made by kblksoftirqd */
	uint64_t ipis_sent; /* completions carried to another CPU */
	uint64_t ipis_recv; /* IPIs taken here */
} __attribute__((aligned(64)));

static struct blk_sirq_cpu g_bsq[MAX_CPUS];

/* blk_softirq_init() has run: the per-CPU state may be addressed through
 * the CPU number (the per-CPU segment exists on every CPU that can take an
 * interrupt from here on). */
static volatile int g_bsq_ready;
/* blk_softirq_start() has run: kblksoftirqd/N exist. */
static volatile int g_bsq_started;

/* ---- cache topology ---------------------------------------------------- */

enum bsq_topo_src {
	BSQ_TOPO_UNKNOWN = 0, /* nothing usable: all CPUs share */
	BSQ_TOPO_LEAF4_L3, /* CPUID 4, level 3 */
	BSQ_TOPO_LEAF4_L2, /* CPUID 4, no level 3: level 2 */
	BSQ_TOPO_PACKAGE, /* CPUID 4 absent: the package */
	BSQ_TOPO_AMD_EXT, /* CPUID 0x8000001D, last cache leaf */
	BSQ_TOPO_AMD_CCX, /* AMD 17h models 0-1Fh, Hygon: APIC ID bit 3 */
	BSQ_TOPO_AMD_NODE, /* AMD before 17h: the node */
	BSQ_TOPO_AMD_NO_L3, /* AMD without L3: the core (SMT siblings) */
};

struct bsq_topo {
	enum bsq_topo_src src;
	uint32_t shift; /* APIC ID >> shift = cache identifier */
	uint32_t sharing; /* logical processors the cache reports */
};

/* The boot CPU's view, used for every CPU until it has looked itself. */
static struct bsq_topo g_bsq_topo;

static inline void bsq_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
	__asm__ volatile("cpuid"
			 : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
			 : "a"(leaf), "c"(sub));
}

/* The order of n: the smallest k with 2^k >= n. */
static inline uint32_t bsq_count_order(uint32_t n)
{
	if (n <= 1)
		return 0;
	return 32u - (uint32_t)__builtin_clz(n - 1u);
}

/* Width of the APIC ID field below the package: from the extended
 * topology leaves when the processor has them, else from the logical
 * processor count of leaf 1. */
static uint32_t bsq_package_shift(uint32_t max_leaf)
{
	uint32_t r[4];

	for (uint32_t leaf = 0x1F; leaf >= 0x0B; leaf -= (0x1F - 0x0B)) {
		uint32_t shift = 0;
		int found = 0;

		if (max_leaf < leaf)
			continue;
		for (uint32_t sub = 0; sub < 8; sub++) {
			bsq_cpuid(leaf, sub, r);
			if (((r[2] >> 8) & 0xFF) == 0)
				break;
			shift = r[0] & 0x1F;
			found = 1;
		}
		if (found)
			return shift;
	}
	bsq_cpuid(1, 0, r);
	if (r[3] & (1u << 28)) /* HTT: EBX[23:16] is meaningful */
		return bsq_count_order((r[1] >> 16) & 0xFF);
	return 0;
}

/* Intel-compatible processors: the last cache level CPUID leaf 4 reports
 * (L3, else L2); the package when the leaf is absent. */
static void bsq_detect_leaf4(struct bsq_topo *t, uint32_t max_leaf)
{
	uint32_t r[4];
	uint32_t l2_share = 0, l3_share = 0;

	if (max_leaf < 4) {
		t->src = BSQ_TOPO_PACKAGE;
		t->shift = bsq_package_shift(max_leaf);
		t->sharing = 1u << t->shift;
		return;
	}
	for (uint32_t i = 0; i < 16; i++) {
		uint32_t type, level, sharing;

		bsq_cpuid(4, i, r);
		type = r[0] & 0x1F;
		if (type == 0)
			break;
		level = (r[0] >> 5) & 0x7;
		sharing = ((r[0] >> 14) & 0xFFF) + 1;
		if (level == 2)
			l2_share = sharing;
		else if (level == 3)
			l3_share = sharing;
	}
	if (l3_share) {
		t->src = BSQ_TOPO_LEAF4_L3;
		t->sharing = l3_share;
	} else if (l2_share) {
		t->src = BSQ_TOPO_LEAF4_L2;
		t->sharing = l2_share;
	} else {
		/* Leaf 4 without L2/L3: only SMT can share, and SMT shares
		 * everything -- the package. */
		t->src = BSQ_TOPO_PACKAGE;
		t->shift = bsq_package_shift(max_leaf);
		t->sharing = 1u << t->shift;
		return;
	}
	t->shift = bsq_count_order(t->sharing);
}

/* AMD and Hygon processors. */
static void bsq_detect_amd(struct bsq_topo *t, int hygon)
{
	uint32_t r[4], max_ext, family, model, topoext;

	bsq_cpuid(0x80000000u, 0, r);
	max_ext = r[0];
	bsq_cpuid(1, 0, r);
	family = (r[0] >> 8) & 0xF;
	model = (r[0] >> 4) & 0xF;
	if (family == 0xF) {
		family += (r[0] >> 20) & 0xFF;
		model |= ((r[0] >> 16) & 0xF) << 4;
	}
	topoext = 0;
	if (max_ext >= 0x80000001u) {
		bsq_cpuid(0x80000001u, 0, r);
		topoext = (r[2] >> 22) & 1;
	}

	/* No L3 at all: the cache domain is the core, which SMT siblings
	 * share and nobody else does. */
	if (max_ext < 0x80000006u ||
	    (bsq_cpuid(0x80000006u, 0, r), r[3] == 0)) {
		uint32_t threads = 1;

		if (topoext && max_ext >= 0x8000001Eu) {
			bsq_cpuid(0x8000001Eu, 0, r);
			threads = ((r[1] >> 8) & 0xFF) + 1;
		}
		t->src = BSQ_TOPO_AMD_NO_L3;
		t->sharing = threads;
		t->shift = bsq_count_order(threads);
		return;
	}
	if (hygon || (family == 0x17 && model <= 0x1F)) {
		/* The L3 belongs to a core complex, APIC ID bit 3 up. */
		t->src = BSQ_TOPO_AMD_CCX;
		t->shift = 3;
		t->sharing = 8;
		return;
	}
	if (family < 0x17) {
		/* Before family 17h the L3 is per node.  The node is the part
		 * of the package's core ID field above the cores of one
		 * node. */
		uint32_t core_bits = 0, nodes = 1;

		if (max_ext >= 0x80000008u) {
			bsq_cpuid(0x80000008u, 0, r);
			core_bits = (r[2] >> 12) & 0xF;
			if (core_bits == 0)
				core_bits = bsq_count_order((r[2] & 0xFF) + 1);
		}
		if (topoext && max_ext >= 0x8000001Eu) {
			bsq_cpuid(0x8000001Eu, 0, r);
			nodes = ((r[2] >> 8) & 0x7) + 1;
		}
		t->src = BSQ_TOPO_AMD_NODE;
		t->shift = core_bits > bsq_count_order(nodes) ?
				   core_bits - bsq_count_order(nodes) :
				   0;
		t->sharing = 1u << t->shift;
		return;
	}
	/* Newer families: the last cache leaf of 0x8000001D says how many
	 * logical processors share it. */
	if (topoext && max_ext >= 0x8000001Du) {
		uint32_t last_share = 0;

		for (uint32_t i = 0; i < 16; i++) {
			bsq_cpuid(0x8000001Du, i, r);
			if ((r[0] & 0x1F) == 0)
				break;
			last_share = ((r[0] >> 14) & 0xFFF) + 1;
		}
		if (last_share) {
			t->src = BSQ_TOPO_AMD_EXT;
			t->sharing = last_share;
			t->shift = bsq_count_order(last_share);
			return;
		}
	}
	t->src = BSQ_TOPO_UNKNOWN;
	t->shift = 0;
	t->sharing = 0;
}

/* Detect on the CPU that runs this. */
static void bsq_detect_topology(struct bsq_topo *t)
{
	uint32_t r[4], max_leaf;
	char vendor[13];

	t->src = BSQ_TOPO_UNKNOWN;
	t->shift = 0;
	t->sharing = 0;
	bsq_cpuid(0, 0, r);
	max_leaf = r[0];
	__builtin_memcpy(vendor + 0, &r[1], 4);
	__builtin_memcpy(vendor + 4, &r[3], 4);
	__builtin_memcpy(vendor + 8, &r[2], 4);
	vendor[12] = '\0';
	if (max_leaf == 0)
		return;
	if (__builtin_memcmp(vendor, "AuthenticAMD", 12) == 0) {
		bsq_detect_amd(t, 0);
		return;
	}
	if (__builtin_memcmp(vendor, "HygonGenuine", 12) == 0) {
		bsq_detect_amd(t, 1);
		return;
	}
	/* GenuineIntel, and every other vendor that implements the
	 * deterministic cache parameters leaf the same way. */
	bsq_detect_leaf4(t, max_leaf);
}

/* This CPU's last-level cache identifier. */
static int bsq_llc_id(uint32_t cpu, uint32_t *out)
{
	struct blk_sirq_cpu *c = &g_bsq[cpu];
	percpu_t *pc;

	if (__atomic_load_n(&c->llc_valid, __ATOMIC_ACQUIRE)) {
		*out = c->llc_id;
		return 1;
	}
	pc = percpu_get(cpu);
	if (!pc)
		return 0;
	*out = pc->apic_id >> g_bsq_topo.shift;
	/* Kept once the CPUs are up (the boot CPU's APIC ID is filled in
	 * by SMP set-up).  Same inputs, same result: racing writers store
	 * the same value. */
	if (blk_softirq_running()) {
		c->llc_id = *out;
		__atomic_store_n(&c->llc_valid, 1, __ATOMIC_RELEASE);
	}
	return 1;
}

int blk_cpus_share_cache(uint32_t a, uint32_t b)
{
	uint32_t la, lb;

	if (a == b)
		return 1;
	if (a >= MAX_CPUS || b >= MAX_CPUS)
		return 0;
	if (g_bsq_topo.src == BSQ_TOPO_UNKNOWN)
		return 1;
	if (!bsq_llc_id(a, &la) || !bsq_llc_id(b, &lb))
		return 0;
	return la == lb;
}

/* ---- context ----------------------------------------------------------- */

static inline uint32_t bsq_cpu(void)
{
	return this_cpu_id();
}

int blk_in_softirq(void)
{
	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return 0;
	return g_bsq[bsq_cpu()].active != 0;
}

int blk_in_hardirq(void)
{
	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return 0;
	return g_bsq[bsq_cpu()].hardirq > 0;
}

int blk_softirq_running(void)
{
	return __atomic_load_n(&g_bsq_started, __ATOMIC_ACQUIRE);
}

/* Inside a hard interrupt or a softirq run: a raise is acted on by the
 * interrupt tail or by the run itself. */
static inline int bsq_in_interrupt(const struct blk_sirq_cpu *c)
{
	return c->hardirq > 0 || c->active;
}

void blk_hardirq_enter(void)
{
	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return;
	g_bsq[bsq_cpu()].hardirq++;
	__asm__ volatile("" ::: "memory");
}

void blk_hardirq_exit(void)
{
	struct blk_sirq_cpu *c;

	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return;
	c = &g_bsq[bsq_cpu()];
	__asm__ volatile("" ::: "memory");
	if (WARN_ON_ONCE(c->hardirq <= 0))
		return;
	c->hardirq--;
}

/* ---- done lists -------------------------------------------------------- */

/* Push `e'; 1 when the list was empty (the pusher raises). */
static inline int bsq_list_add(struct blk_softirq_entry *e,
			       struct blk_softirq_entry *volatile *head)
{
	struct blk_softirq_entry *first =
		__atomic_load_n(head, __ATOMIC_RELAXED);

	do {
		e->next = first;
	} while (!__atomic_compare_exchange_n(head, &first, e, 1,
					      __ATOMIC_RELEASE,
					      __ATOMIC_RELAXED));
	return first == NULL;
}

/* Take the whole list and return it oldest first. */
static inline struct blk_softirq_entry *
bsq_list_take_in_order(struct blk_softirq_entry *volatile *head)
{
	struct blk_softirq_entry *e =
		__atomic_exchange_n(head, NULL, __ATOMIC_ACQUIRE);
	struct blk_softirq_entry *rev = NULL;

	while (e) {
		struct blk_softirq_entry *next = e->next;

		e->next = rev;
		rev = e;
		e = next;
	}
	return rev;
}

/* ---- raising ----------------------------------------------------------- */

/* Wake this CPU's kblksoftirqd if it is parked.  Interrupts off, on the
 * CPU that owns `c'.  It is claimed directly (sched_claim_wake +
 * sched_enqueue_ready) like every single-task waker that may run in an
 * interrupt; the enqueue asks for a reschedule when the thread should
 * have the processor now. */
static void bsq_wakeup_thread(struct blk_sirq_cpu *c)
{
	task_t *t = c->thread;

	if (t && t->wait_channel == (void *)c &&
	    sched_claim_wake(t, TASK_BLOCKED)) {
		t->wait_channel = NULL;
		t->wakeup_tick = 0;
		sched_enqueue_ready(t);
	}
}

/* Raise on this CPU.  Interrupts off.  The pending store is a full
 * barrier: it is ordered before the parked check in bsq_wakeup_thread,
 * which pairs with the thread publishing "blocked" before it re-reads
 * `pending'. */
static inline void bsq_raise_irqoff(struct blk_sirq_cpu *c)
{
	(void)__atomic_exchange_n(&c->pending, 1, __ATOMIC_SEQ_CST);
	if (!bsq_in_interrupt(c))
		bsq_wakeup_thread(c);
}

/* ---- running ----------------------------------------------------------- */

/* Finish everything on this CPU's done list, oldest first.  `irqs_on':
 * the state the entries are given (and must hand back). */
static void bsq_done_softirq(struct blk_sirq_cpu *c, uint32_t cpu,
			     int irqs_on)
{
	struct blk_softirq_entry *e = bsq_list_take_in_order(&c->done);

	while (e) {
		struct blk_softirq_entry *next = e->next;

		e->next = NULL;
		e->fn(e);
		c->entries++;
		/* An entry that switched tasks moved this run off its CPU:
		 * the run's state now belongs to another processor. */
		BUG_ON(bsq_cpu() != cpu);
		/* An entry must hand back what it was given. */
		if (WARN_ON_ONCE(irqs_enabled() != irqs_on)) {
			if (irqs_on)
				local_irq_restore(local_irq_save() | RFLAGS_IF);
			else
				(void)local_irq_save();
		}
		e = next;
	}
}

/* One run of the block softirq on this CPU.  Called with interrupts off;
 * `flags' is the caller's saved RFLAGS (interrupts off in it or not), from
 * which the run derives the state its entries run in: interrupts on,
 * unless `keep_irqs_off' (a poller that must not take interrupts). */
static void bsq_handle(struct blk_sirq_cpu *c, uint32_t cpu, uint64_t flags,
		       int keep_irqs_off)
{
	uint64_t end = timer_ticks() + timer_ms_to_ticks(BLK_SOFTIRQ_MAX_TIME_MS);
	int max_restart = BLK_SOFTIRQ_MAX_RESTART;
	uint32_t pending = c->pending;

	c->active = 1;
	c->runs++;
	__asm__ volatile("" ::: "memory");
restart:
	/* Cleared before interrupts go on: whatever is raised from here
	 * is seen below. */
	__atomic_store_n(&c->pending, 0, __ATOMIC_SEQ_CST);
	if (!keep_irqs_off)
		local_irq_restore(flags | RFLAGS_IF);
	if (pending)
		bsq_done_softirq(c, cpu, !keep_irqs_off);
	(void)local_irq_save();
	pending = __atomic_load_n(&c->pending, __ATOMIC_SEQ_CST);
	if (pending) {
		if ((int64_t)(timer_ticks() - end) < 0 &&
		    !sched_need_resched() && --max_restart) {
			c->restarts++;
			goto restart;
		}
		c->handovers++;
		bsq_wakeup_thread(c);
	}
	__asm__ volatile("" ::: "memory");
	c->active = 0;
}

void blk_softirq_irq_tail(void)
{
	struct blk_sirq_cpu *c;
	uint64_t fl;
	uint32_t cpu;

	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return;
	fl = local_irq_save();
	cpu = bsq_cpu();
	c = &g_bsq[cpu];
	/* Only the outermost interrupt runs it, and only when it did not
	 * interrupt a run. */
	if (c->hardirq == 0 && !c->active &&
	    __atomic_load_n(&c->pending, __ATOMIC_ACQUIRE))
		bsq_handle(c, cpu, fl, 0);
	local_irq_restore(fl);
}

void blk_softirq_poll(void)
{
	struct blk_sirq_cpu *c;
	uint64_t fl;
	uint32_t cpu;

	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return;
	fl = local_irq_save();
	cpu = bsq_cpu();
	c = &g_bsq[cpu];
	if (bsq_in_interrupt(c)) {
		local_irq_restore(fl);
		return;
	}
	/* A poller with interrupts off cannot take the IPI that raises the
	 * softirq for completions another CPU put on this CPU's list: an
	 * entry on the list is reason enough to run.  (The IPI, once it is
	 * taken, raises a run that finds the list empty.) */
	if (__atomic_load_n(&c->done, __ATOMIC_ACQUIRE))
		(void)__atomic_exchange_n(&c->pending, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&c->pending, __ATOMIC_ACQUIRE))
		bsq_handle(c, cpu, fl, !(fl & RFLAGS_IF));
	local_irq_restore(fl);
}

/* The block IPI: another CPU put completions on this CPU's done list.
 * The softirq runs at this interrupt's tail. */
void blk_softirq_ipi(void)
{
	struct blk_sirq_cpu *c;

	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return;
	c = &g_bsq[bsq_cpu()];
	c->ipis_recv++;
	bsq_raise_irqoff(c);
}

/* ---- completion -------------------------------------------------------- */

static inline int bsq_cpu_online(uint32_t cpu)
{
	return cpu < smp_get_cpu_count() && cpu < MAX_CPUS &&
	       percpu_get(cpu) != NULL;
}

/* Whether a completion taken on `cpu' for a request submitted on `target'
 * must be carried to `target'. */
static int bsq_need_ipi(uint32_t cpu, int target, unsigned int flags)
{
	if (smp_get_cpu_count() <= 1)
		return 0;
	if (!(flags & BLK_SOFTIRQ_SAME_COMP) || target < 0)
		return 0;
	/* Same CPU, or the same cache domain (all CPUs have the same
	 * capacity here): finish locally. */
	if (cpu == (uint32_t)target ||
	    (!(flags & BLK_SOFTIRQ_SAME_FORCE) &&
	     blk_cpus_share_cache(cpu, (uint32_t)target)))
		return 0;
	/* Never at a CPU that is not running. */
	return bsq_cpu_online((uint32_t)target);
}

/* Interrupts off. */
static void bsq_send_ipi(struct blk_softirq_entry *e, uint32_t target)
{
	struct blk_sirq_cpu *t = &g_bsq[target];

	if (bsq_list_add(e, &t->done)) {
		g_bsq[bsq_cpu()].ipis_sent++;
		smp_send_block_softirq(target);
	}
}

/* Interrupts off. */
static void bsq_raise_local(struct blk_softirq_entry *e,
			    struct blk_sirq_cpu *c)
{
	if (bsq_list_add(e, &c->done))
		bsq_raise_irqoff(c);
}

static inline int bsq_check_entry(struct blk_softirq_entry *e)
{
	BUG_ON(e == NULL);
	if (WARN_ON_ONCE(e->fn == NULL))
		return 0;
	return 1;
}

int blk_softirq_complete_remote(struct blk_softirq_entry *e, int submit_cpu,
				unsigned int flags)
{
	struct blk_sirq_cpu *c;
	uint32_t cpu, ncpus, nr_ctx_one;
	uint64_t fl;
	int ret = 1;

	if (!bsq_check_entry(e))
		return 1;
	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return 0;
	fl = local_irq_save();
	cpu = bsq_cpu();
	c = &g_bsq[cpu];
	/* Before kblksoftirqd exists a process-context caller is the only
	 * context that will ever run it: the caller completes. */
	if (!blk_softirq_running() && !bsq_in_interrupt(c)) {
		local_irq_restore(fl);
		return 0;
	}
	/* A hardware queue served by this CPU alone, or a polled request:
	 * completing elsewhere gains nothing. */
	ncpus = smp_get_cpu_count();
	if (flags & BLK_SOFTIRQ_MULTI_HWQ)
		nr_ctx_one = (flags & BLK_SOFTIRQ_HCTX_ONE_CTX) != 0;
	else
		nr_ctx_one = ncpus <= 1;
	if ((nr_ctx_one && submit_cpu == (int)cpu) ||
	    (flags & BLK_SOFTIRQ_POLLED)) {
		ret = 0;
	} else if (bsq_need_ipi(cpu, submit_cpu, flags)) {
		bsq_send_ipi(e, (uint32_t)submit_cpu);
	} else if (!(flags & BLK_SOFTIRQ_MULTI_HWQ)) {
		bsq_raise_local(e, c);
	} else {
		ret = 0;
	}
	local_irq_restore(fl);
	return ret;
}

void blk_softirq_complete(struct blk_softirq_entry *e, int submit_cpu)
{
	unsigned int flags = submit_cpu >= 0 ? BLK_SOFTIRQ_SAME_COMP : 0;
	struct blk_sirq_cpu *c;
	uint64_t fl;

	if (!bsq_check_entry(e))
		return;
	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE)) {
		/* Before the framework exists there is no interrupt that
		 * could be completing anything: the caller's context. */
		e->next = NULL;
		e->fn(e);
		return;
	}
	if (blk_softirq_complete_remote(e, submit_cpu, flags))
		return;
	/* The caller would complete inline.  Before kblksoftirqd exists
	 * that is right for a process-context caller; otherwise this
	 * interface never runs the entry synchronously, so it is raised on
	 * this CPU. */
	fl = local_irq_save();
	c = &g_bsq[bsq_cpu()];
	if (!blk_softirq_running() && !bsq_in_interrupt(c)) {
		local_irq_restore(fl);
		e->next = NULL;
		e->fn(e);
		return;
	}
	bsq_raise_local(e, c);
	local_irq_restore(fl);
}

void blk_softirq_complete_local(struct blk_softirq_entry *e)
{
	struct blk_sirq_cpu *c;
	uint64_t fl;

	if (!bsq_check_entry(e))
		return;
	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE)) {
		e->next = NULL;
		e->fn(e);
		return;
	}
	fl = local_irq_save();
	c = &g_bsq[bsq_cpu()];
	if (!blk_softirq_running() && !bsq_in_interrupt(c)) {
		local_irq_restore(fl);
		e->next = NULL;
		e->fn(e);
		return;
	}
	bsq_raise_local(e, c);
	local_irq_restore(fl);
}

/* ---- kblksoftirqd ------------------------------------------------------ */

static void kblksoftirqd_main(void *arg)
{
	uint32_t cpu = (uint32_t)(uintptr_t)arg;
	struct blk_sirq_cpu *c;
	struct bsq_topo own;

	BUG_ON(cpu >= MAX_CPUS);
	c = &g_bsq[cpu];
	WARN_ON_ONCE(bsq_cpu() != cpu); /* pinned at creation */

	/* This CPU's own cache parameters, now that code runs on it. */
	bsq_detect_topology(&own);
	if (own.src != BSQ_TOPO_UNKNOWN && g_bsq_topo.src != BSQ_TOPO_UNKNOWN) {
		percpu_t *pc = percpu_get(cpu);

		if (pc) {
			c->llc_id = pc->apic_id >> own.shift;
			__atomic_store_n(&c->llc_valid, 1, __ATOMIC_RELEASE);
		}
	}

	for (;;) {
		task_t *cur = sched_current();
		uint64_t fl;

		/* Park unless work is pending.  "Blocked" is published
		 * before `pending' is read (sequentially consistent), and a
		 * raiser stores `pending' before it looks for a parked
		 * thread: one of the two sees the other. */
		cur->wait_channel = (void *)c;
		task_set_blocked(cur);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (!__atomic_load_n(&c->pending, __ATOMIC_SEQ_CST)) {
			sched_schedule();
			cur->wait_channel = NULL;
			continue;
		}
		{
			/* Un-park by claiming ourselves back, the mirror of the
			 * waker's claim.  When a waker won, this task is READY
			 * and queued (or about to be): pass through the
			 * scheduler, which either keeps it here or resumes it
			 * from the queue. */
			task_state_t expected = TASK_BLOCKED;

			if (!__atomic_compare_exchange_n(&cur->state, &expected,
							 TASK_RUNNING, 0,
							 __ATOMIC_ACQ_REL,
							 __ATOMIC_ACQUIRE))
				sched_schedule();
		}
		cur->wait_channel = NULL;

		fl = local_irq_save();
		if (!c->active && __atomic_load_n(&c->pending, __ATOMIC_ACQUIRE)) {
			c->thread_runs++;
			bsq_handle(c, cpu, fl, 0);
		}
		local_irq_restore(fl);
		/* Between runs the thread is an ordinary task. */
		if (sched_need_resched())
			sched_yield_in_kernel();
	}
}

/* ---- set-up ------------------------------------------------------------ */

void blk_softirq_init(void)
{
	BUILD_BUG_ON(sizeof(struct blk_sirq_cpu) % 64 != 0);
	/* Above every device vector, below the scheduler's IPIs. */
	BUILD_BUG_ON(IPI_BLOCK_SOFTIRQ_VECTOR <= IRQ_DYN_VECTOR_LAST);
	BUILD_BUG_ON(IPI_BLOCK_SOFTIRQ_VECTOR >= IPI_RESCHEDULE_VECTOR);
	/* Interrupts may already be on here (drivers probed before this
	 * one enable them on the boot CPU).  That is harmless: until the
	 * ready flag below is published the interrupt entry and exit hooks
	 * do nothing, and an interrupt taken after it runs to its end on
	 * this CPU before the caller resumes, so its enter/exit pair always
	 * sees the same flag.  No storage interrupt is set up yet. */

	if (__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE))
		return;
	for (uint32_t i = 0; i < MAX_CPUS; i++) {
		g_bsq[i].done = NULL;
		g_bsq[i].pending = 0;
		g_bsq[i].active = 0;
		g_bsq[i].hardirq = 0;
		g_bsq[i].thread = NULL;
		g_bsq[i].llc_valid = 0;
	}
	bsq_detect_topology(&g_bsq_topo);
	kblockd_init();
	__atomic_store_n(&g_bsq_ready, 1, __ATOMIC_RELEASE);
}

static void bsq_name_thread(task_t *t, const char *base, uint32_t cpu)
{
	ksnprintf(t->comm, sizeof(t->comm), "%s/%u", base, cpu);
}

void blk_softirq_start(void)
{
	uint32_t ncpus = smp_get_cpu_count(), started = 0;

	might_sleep();
	if (!__atomic_load_n(&g_bsq_ready, __ATOMIC_ACQUIRE)) {
		WARN(1, BLK_LOG_PREFIX "blk_softirq_start before blk_softirq_init");
		return;
	}
	if (blk_softirq_running())
		return;
	if (sched_current() == NULL) {
		WARN(1, BLK_LOG_PREFIX "block softirq threads started before the scheduler");
		return;
	}
	if (ncpus == 0)
		ncpus = 1;
	if (ncpus > MAX_CPUS)
		ncpus = MAX_CPUS;
	for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
		void *stack;
		task_t *t;

		if (!percpu_get(cpu))
			continue;
		/* Guarded: a run of the softirq here is as deep as one at an
		 * interrupt tail, plus a nested interrupt. */
		stack = mm_alloc_guarded_kstack(KBLKSOFTIRQD_STACK_SIZE);
		if (!stack) {
			kprintf(BLK_LOG_PREFIX
				"kblksoftirqd/%u: no stack; that CPU's block softirq runs only at interrupt tails\n",
				cpu);
			continue;
		}
		/* Pinned at creation: it serves one CPU's done list. */
		t = sched_add_task_on_cpu(kblksoftirqd_main,
					  (void *)(uintptr_t)cpu, stack,
					  KBLKSOFTIRQD_STACK_SIZE, cpu);
		if (!t) {
			mm_free_guarded_kstack(stack, KBLKSOFTIRQD_STACK_SIZE);
			kprintf(BLK_LOG_PREFIX "kblksoftirqd/%u: cannot create the thread\n",
				cpu);
			continue;
		}
		bsq_name_thread(t, "kblksoftirqd", cpu);
		__atomic_store_n(&g_bsq[cpu].thread, t, __ATOMIC_RELEASE);
		started++;
	}
	/* The per-CPU kblockd workers. */
	blk_kblockd_start();
	__atomic_store_n(&g_bsq_started, 1, __ATOMIC_RELEASE);
}
