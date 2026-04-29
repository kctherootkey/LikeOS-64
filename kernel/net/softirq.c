// LikeOS-64 — Deferred-work (softirq) framework
//
// See include/kernel/softirq.h for design rationale.

#include "../../include/kernel/softirq.h"
#include "../../include/kernel/percpu.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/smp.h"
#include "../../include/kernel/console.h"

static softirq_fn_t   softirq_handlers[NR_SOFTIRQ];
static volatile uint32_t softirq_pending_mask[MAX_CPUS];
static volatile int      softirq_in_progress[MAX_CPUS];
static task_t*           ksoftirqd_task[MAX_CPUS];

// Before percpu_init() runs, GS base is 0 and this_cpu_id() would fault
// reading %gs:cpu_id_offset.  IRQs (PS/2 mouse, keyboard, timer) can fire
// during early init from the BSP — in that case we are always CPU 0.
static inline uint32_t safe_cpu_id(void) {
    return read_gs_base_msr() ? this_cpu_id() : 0;
}

void softirq_register(uint32_t nr, softirq_fn_t fn) {
    if (nr >= NR_SOFTIRQ) return;
    softirq_handlers[nr] = fn;
}

static inline void softirq_wake_local(uint32_t cpu) {
    task_t* t = ksoftirqd_task[cpu];
    if (t && t->state == TASK_BLOCKED && t->wait_channel == (void*)&ksoftirqd_task[cpu]) {
        sched_wake_channel((void*)&ksoftirqd_task[cpu]);
    }
}

void softirq_raise(uint32_t nr) {
    if (nr >= NR_SOFTIRQ) return;
    uint32_t cpu = safe_cpu_id();
    __atomic_fetch_or(&softirq_pending_mask[cpu], (1u << nr), __ATOMIC_ACQ_REL);
    softirq_wake_local(cpu);
}

void softirq_raise_on(uint32_t cpu, uint32_t nr) {
    if (nr >= NR_SOFTIRQ || cpu >= MAX_CPUS) return;
    __atomic_fetch_or(&softirq_pending_mask[cpu], (1u << nr), __ATOMIC_ACQ_REL);
    // sched_wake_channel works cross-CPU; the target CPU's scheduler will
    // pick up the runnable ksoftirqd on its next preemption.  Latency is at
    // most one scheduler tick (10 ms at 100 Hz), but the IRQ-tail drain on
    // any CPU also picks up bits from cpu==target if it later runs there.
    softirq_wake_local(cpu);
}

// Returns the previous IF state (1 if IRQs were enabled, 0 otherwise) and
// disables IRQs.
static inline uint64_t local_irq_save_raw(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void local_irq_restore_raw(uint64_t f) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

void softirq_drain(void) {
    uint64_t saved = local_irq_save_raw();
    uint32_t cpu = safe_cpu_id();

    if (softirq_in_progress[cpu]) {
        // Already draining on this CPU (re-entered from IRQ tail while a
        // handler is running).  The outer drain will pick up newly-raised
        // bits on its next loop iteration.
        local_irq_restore_raw(saved);
        return;
    }

    // Only re-enable IRQs around handler execution if the CALLER had them
    // enabled (i.e. we were invoked from task / kernel-thread context).
    //
    // When called from IRQ-tail (NIC IRQ -> softirq_drain), saved IF == 0.
    // We MUST keep IRQs disabled in that case: enabling them would allow
    // the 0xFE reschedule IPI's ipi_handler() to invoke sched_preempt()
    // while we are still running on the per-CPU IRQ stack with a
    // half-completed handler frame -- corrupting the IRQ stack and
    // leaving the CPU in a state where it stops servicing subsequent
    // IPIs (manifests as `TLB shootdown sync timeout (ack=N expect=N+1)`).
    //
    // Hard-IRQ-tail drains therefore run with IRQs disabled.  Any work
    // they cannot finish quickly is left on the pending mask for the
    // per-CPU ksoftirqd kernel thread, which DOES run in a preemptible
    // context and will sti around handlers safely.
    int caller_had_irqs = (saved & (1ULL << 9)) != 0;

    softirq_in_progress[cpu] = 1;

    for (;;) {
        uint32_t mask = __atomic_exchange_n(&softirq_pending_mask[cpu], 0,
                                            __ATOMIC_ACQ_REL);
        if (mask == 0) break;

        if (caller_had_irqs) {
            __asm__ volatile ("sti" ::: "memory");
        }
        for (uint32_t i = 0; i < NR_SOFTIRQ; i++) {
            if (mask & (1u << i)) {
                softirq_fn_t h = softirq_handlers[i];
                if (h) h();
            }
        }
        if (caller_had_irqs) {
            __asm__ volatile ("cli" ::: "memory");
        }
    }

    softirq_in_progress[cpu] = 0;
    local_irq_restore_raw(saved);
}

// ---- ksoftirqd kernel thread ----

static void ksoftirqd_main(void* arg) {
    uint32_t my_cpu = (uint32_t)(uintptr_t)arg;

    for (;;) {
        softirq_drain();

        // Race-free sleep: mark blocked, then re-check pending, then yield.
        // If softirq_raise() races in between, it will set the bit; our
        // re-check sees it and we loop without sleeping.  If it raises after
        // we yield, sched_wake_channel() puts us back on the runqueue.
        task_t* cur = sched_current();
        cur->wait_channel = (void*)&ksoftirqd_task[my_cpu];
        cur->state = TASK_BLOCKED;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (softirq_pending_mask[my_cpu] != 0) {
            cur->state = TASK_RUNNING;
            cur->wait_channel = NULL;
            continue;
        }
        sched_yield_in_kernel();
    }
}

void ksoftirqd_start_all(void) {
    uint32_t ncpus = smp_get_cpu_count();
    if (ncpus == 0) ncpus = 1;
    for (uint32_t cpu = 0; cpu < ncpus && cpu < MAX_CPUS; cpu++) {
        task_t* t = sched_add_user_task(ksoftirqd_main,
                                        (void*)(uintptr_t)cpu,
                                        NULL, 0, 0);
        if (!t) {
            kprintf("ksoftirqd: failed to create thread for CPU %u\n", cpu);
            continue;
        }
        t->on_cpu        = cpu;
        t->cpu_affinity  = (1ULL << cpu);
        ksoftirqd_task[cpu] = t;
    }
    kprintf("softirq: ksoftirqd started on %u CPUs\n", ncpus);
}
