// LikeOS-64 — Deferred-work (softirq) framework
//
// See include/kernel/softirq.h for design rationale.

#include <kernel/net/softirq.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/smp.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

static softirq_fn_t softirq_handlers[NR_SOFTIRQ];
static volatile uint32_t softirq_pending_mask[MAX_CPUS];
static volatile int softirq_in_progress[MAX_CPUS];
static task_t *ksoftirqd_task[MAX_CPUS];

// Before percpu_init() runs, GS base is 0 and this_cpu_id() would fault
// reading %gs:cpu_id_offset.  IRQs (PS/2 mouse, keyboard, timer) can fire
// during early init from the BSP — in that case we are always CPU 0.
static inline uint32_t safe_cpu_id(void)
{
	return read_gs_base_msr() ? this_cpu_id() : 0;
}

void softirq_register(uint32_t nr, softirq_fn_t fn)
{
	BUG_ON(fn == NULL);
	if (nr >= NR_SOFTIRQ)
		return;
	WARN_ON(softirq_handlers[nr] !=
		NULL); /* double-registration of softirq handler */
	softirq_handlers[nr] = fn;
}

static inline void softirq_wake_local(uint32_t cpu)
{
	task_t *t = ksoftirqd_task[cpu];
	if (t && t->state == TASK_BLOCKED &&
	    t->wait_channel == (void *)&ksoftirqd_task[cpu]) {
		sched_wake_channel((void *)&ksoftirqd_task[cpu]);
	}
}

void softirq_raise(uint32_t nr)
{
	if (nr >= NR_SOFTIRQ)
		return;
	uint32_t cpu = safe_cpu_id();
	__atomic_fetch_or(&softirq_pending_mask[cpu], (1u << nr),
			  __ATOMIC_ACQ_REL);
	softirq_wake_local(cpu);
}

void softirq_raise_on(uint32_t cpu, uint32_t nr)
{
	WARN_RATELIMIT(nr >= NR_SOFTIRQ, "softirq nr %u out of range (max %u)",
		       nr, NR_SOFTIRQ);
	if (nr >= NR_SOFTIRQ || cpu >= MAX_CPUS)
		return;
	__atomic_fetch_or(&softirq_pending_mask[cpu], (1u << nr),
			  __ATOMIC_ACQ_REL);
	/* Wake ksoftirqd on the target CPU and force a reschedule there.
     * Without the IPI, ksoftirqd just sits in the target CPU's run queue
     * until the next 10 ms timer tick picks it up — which throttles
     * network RX to ~100 pkt/s (the agent's analysis identified this
     * as a primary cause of the 300 KB/s ceiling).  The IPI makes the
     * target CPU exit halt/userspace immediately and run the scheduler. */
	softirq_wake_local(cpu);
	if (cpu != safe_cpu_id() && smp_is_enabled()) {
		smp_send_reschedule(cpu);
	}
}

/* Read-only diagnostic accessors used by the Ctrl+N TCP/net dump
 * (tcp_dump_table) to detect a softirq lost wakeup.  No locks, no
 * side effects. */
uint32_t softirq_pending_get(uint32_t cpu)
{
	if (cpu >= MAX_CPUS)
		return 0;
	return softirq_pending_mask[cpu];
}

int ksoftirqd_state_get(uint32_t cpu)
{
	if (cpu >= MAX_CPUS)
		return -1;
	task_t *t = ksoftirqd_task[cpu];
	return t ? (int)t->state : -1;
}

// Returns the previous IF state (1 if IRQs were enabled, 0 otherwise) and
// disables IRQs.
static inline uint64_t local_irq_save_raw(void)
{
	uint64_t f;
	__asm__ volatile("pushfq; popq %0; cli" : "=r"(f)::"memory");
	return f;
}

static inline void local_irq_restore_raw(uint64_t f)
{
	__asm__ volatile("pushq %0; popfq" ::"r"(f) : "memory", "cc");
}

void softirq_drain(void)
{
	uint64_t saved = local_irq_save_raw();
	uint32_t cpu = safe_cpu_id();

	if (softirq_in_progress[cpu]) {
		// Already draining on this CPU (re-entered from IRQ tail while a
		// handler is running).  The outer drain will pick up newly-raised
		// bits on its next loop iteration.
		local_irq_restore_raw(saved);
		return;
	}

	int caller_had_irqs = (saved & (1ULL << 9)) != 0;

	// CRITICAL: when invoked from hard-IRQ tail (caller_had_irqs == 0) we
	// MUST NOT execute softirq handlers here.  Two failure modes:
	//
	//   1. Running handlers with IRQs enabled on the per-CPU IRQ stack
	//      lets a 0xFE reschedule IPI invoke sched_preempt() with a
	//      half-completed handler frame -- corrupts IRQ stack, CPU stops
	//      servicing subsequent IPIs.
	//
	//   2. Running handlers with IRQs DISABLED holds the CPU off-IRQ for
	//      potentially milliseconds (NET_RX softirq draining many TCP
	//      segments, taking conn->lock, etc.).  During that window the
	//      CPU cannot ACK TLB-shootdown IPIs from another CPU doing
	//      slab_free() in process context -- manifests as `SMP: TLB
	//      shootdown sync timeout (ack=N expect=N+1)` and a multi-second
	//      OS-wide freeze.
	//
	// Solution: from IRQ tail, leave the pending bits set and just wake
	// ksoftirqd on this CPU.  ksoftirqd runs in process context with
	// IRQs enabled and is safely preemptible / yields when needed.
	if (!caller_had_irqs) {
		softirq_wake_local(cpu);
		local_irq_restore_raw(saved);
		return;
	}

	softirq_in_progress[cpu] = 1;
	BUG_ON(cpu >= MAX_CPUS);

	for (;;) {
		uint32_t mask = __atomic_exchange_n(&softirq_pending_mask[cpu],
						    0, __ATOMIC_ACQ_REL);
		if (mask == 0)
			break;

		__asm__ volatile("sti" ::: "memory");
		for (uint32_t i = 0; i < NR_SOFTIRQ; i++) {
			if (mask & (1u << i)) {
				softirq_fn_t h = softirq_handlers[i];
				if (h)
					h();
			}
		}
		__asm__ volatile("cli" ::: "memory");
	}

	softirq_in_progress[cpu] = 0;
	local_irq_restore_raw(saved);
}

// ---- ksoftirqd kernel thread ----

static void ksoftirqd_main(void *arg)
{
	uint32_t my_cpu = (uint32_t)(uintptr_t)arg;
	BUG_ON(my_cpu >= MAX_CPUS);

	for (;;) {
		softirq_drain();

		// Race-free sleep: mark blocked, then re-check pending, then yield.
		// If softirq_raise() races in between, it will set the bit; our
		// re-check sees it and we loop without sleeping.  If it raises after
		// we yield, sched_wake_channel() puts us back on the runqueue.
		//
		// NOTE: must call sched_schedule() directly here, NOT
		// sched_yield_in_kernel(), because the latter unconditionally sets
		// state = TASK_READY before scheduling and would defeat the BLOCKED
		// marking — making ksoftirqd a 100% CPU busy-loop on every CPU,
		// which starves other ready tasks (visible as multi-second OS-wide
		// freezes when ksoftirqd holds runqueue locks contended cross-CPU).
		task_t *cur = sched_current();
		cur->wait_channel = (void *)&ksoftirqd_task[my_cpu];
		cur->state = TASK_BLOCKED;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (softirq_pending_mask[my_cpu] != 0) {
			/* Un-park by CLAIMING ourselves back with a CAS — the
			 * mirror of sched_claim_wake() on the waker side.  The
			 * previous blind `state = TASK_RUNNING` write raced
			 * softirq_wake_local's claim: the waker could CAS
			 * BLOCKED→READY first, then find state == RUNNING at
			 * rq_enqueue_locked — a "BUG: enqueue RUNNING" warning
			 * per packet once loopback started streaming at full
			 * rate (the console flood alone destabilizes the box). */
			task_state_t expected = TASK_BLOCKED;
			if (__atomic_compare_exchange_n(&cur->state, &expected,
							TASK_RUNNING, false,
							__ATOMIC_ACQ_REL,
							__ATOMIC_ACQUIRE)) {
				cur->wait_channel = NULL;
				continue;
			}
			/* A waker won the claim: we are READY and it enqueues
			 * us (it already cleared wait_channel).  Do NOT write
			 * RUNNING over the claim (that refused the waker's
			 * enqueue and spammed "BUG: enqueue RUNNING"), and do
			 * NOT wait for on_rq — a third CPU's transient
			 * dequeue/re-enqueue of the sp==0 entry, or a timer
			 * preempt cycling this task, makes that wait
			 * unbounded.  Fall through to sched_schedule() as a
			 * READY current: it either dequeues us straight back
			 * (next == cur → stay on CPU) or parks us READY and
			 * the queue entry resumes us; sched_enqueue_ready
			 * dedups silently via on_rq if both sides enqueue. */
		}
		sched_schedule();
		cur->wait_channel = NULL;
	}
}

void ksoftirqd_start_all(void)
{
	might_sleep();
	uint32_t ncpus = smp_get_cpu_count();
	if (ncpus == 0)
		ncpus = 1;
	WARN_ON_ONCE(
		ncpus >
		MAX_CPUS); /* smp_get_cpu_count() returned more CPUs than MAX_CPUS: ACPI/SMP topology exceeds compile-time limit */
	uint32_t started = 0;
	/* ksoftirqd runs entirely in kernel mode (calls softirq_drain,
     * sched_yield_in_kernel) so it must be a TASK_KERNEL thread, not a
     * user task.  sched_add_user_task requires a non-NULL pml4 and would
     * fail unconditionally here.  Use sched_add_task with a per-thread
     * kalloc'd kernel stack. */
	const size_t KSOFTIRQD_STACK_SIZE = 16 * 1024;
	for (uint32_t cpu = 0; cpu < ncpus && cpu < MAX_CPUS; cpu++) {
		/* Guarded kstack (guard page below): ksoftirqd runs the WHOLE
		 * network RX path — softirq_drain -> net_rx_softirq -> ipv4_rx
		 * -> tcp_rx -> ... -> tcp_send/ipv4_send, plus nested hard IRQs
		 * — on this stack, the deepest kernel-thread stack usage in the
		 * system.  A plain kalloc'd stack has no guard page, so an
		 * overflow would silently clobber adjacent heap (a task_t, a
		 * malloc chunk) and surface later as unrelated corruption; the
		 * guard page turns that into a clean fault at the overflow. */
		void *stack = mm_alloc_guarded_kstack(KSOFTIRQD_STACK_SIZE);
		if (!stack) {
			kprintf("ksoftirqd: failed to alloc stack for CPU %u\n",
				cpu);
			continue;
		}
		task_t *t =
			sched_add_task(ksoftirqd_main, (void *)(uintptr_t)cpu,
				       stack, KSOFTIRQD_STACK_SIZE);
		if (!t) {
			kprintf("ksoftirqd: failed to create thread for CPU %u\n",
				cpu);
			mm_free_guarded_kstack(stack, KSOFTIRQD_STACK_SIZE);
			continue;
		}
		t->on_cpu = cpu;
		t->cpu_affinity = (1ULL << cpu);
		/* Name shown by ps as "[ksoftirqd/N]" (kernel-thread bracket added
         * by ps when t->privilege == TASK_KERNEL). */
		{
			const char *p = "ksoftirqd/";
			int i = 0;
			while (p[i]) {
				t->comm[i] = p[i];
				i++;
			}
			if (cpu >= 10) {
				t->comm[i++] = (char)('0' + (cpu / 10));
			}
			t->comm[i++] = (char)('0' + (cpu % 10));
			t->comm[i] = '\0';
		}
		ksoftirqd_task[cpu] = t;
		started++;
	}
	kprintf("softirq: ksoftirqd started on %u of %u CPUs\n", started,
		ncpus);
}
