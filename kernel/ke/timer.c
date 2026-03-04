// LikeOS-64 PIT Timer Driver
#include "../../include/kernel/timer.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/signal.h"
#include "../../include/kernel/percpu.h"

static volatile uint64_t g_ticks = 0;
static uint32_t g_frequency = 100; // Default 100 Hz

void timer_init(uint32_t frequency_hz) {
    if (frequency_hz < 19 || frequency_hz > 1193182) {
        frequency_hz = 100; // Clamp to safe default
    }
    g_frequency = frequency_hz;

    // Calculate divisor for desired frequency
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;

    // Channel 0, lobyte/hibyte, rate generator mode
    outb(PIT_CMD, 0x36);

    // Send divisor low/high bytes
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    kprintf("PIT timer initialized at %u Hz (divisor=%u)\n", frequency_hz, divisor);
}

void timer_start(void) {
    irq_enable(0); // Enable IRQ0
}

void timer_stop(void) {
    irq_disable(0); // Disable IRQ0
}

uint64_t timer_ticks(void) {
    return g_ticks;
}

void timer_irq_handler(void) {
    // Determine if we're on BSP or AP
    // Only BSP (CPU 0) manages global tick counter and task wakeups.
    // APs receive this via LAPIC timer at the same vector but only
    // manage their own per-CPU time slice tracking.
    int is_bsp = 1;
    if (sched_is_smp()) {
        is_bsp = (this_cpu_id() == 0);
    }
    
    if (is_bsp) {
        g_ticks++;
        
        // Wake any tasks whose sleep timer has expired and check signal timers
        // This handles alarm(), itimer, and wakes sleeping tasks
        sched_wake_expired_sleepers(g_ticks);
    }
    
    // Per-CPU: manage this CPU's current task time slice
    task_t* cur = sched_current();
    if (cur) {
        if (cur->remaining_ticks > 0) {
            cur->remaining_ticks--;
        }
        // Always request preemption when the timeslice expires, regardless
        // of task state.  In particular, a ZOMBIE task (killed by signal
        // while running) sits in an `sti; hlt` loop waiting to be preempted
        // off this CPU.  If we only checked RUNNING/READY here, the ZOMBIE
        // would never get need_resched set, sched_preempt would never be
        // called, and sched_remove_task on another CPU would spin forever
        // waiting for this CPU to context-switch away.
        if (cur->remaining_ticks == 0) {
            sched_set_need_resched(cur);
        }
    }
    
    // Per-CPU load balancing: periodically pull tasks from busiest CPU
    // Run every LOAD_BALANCE_INTERVAL ticks (checked via global tick counter)
    if (sched_is_smp() && (g_ticks % 50) == 0) {
        sched_load_balance();
    }
    
    // Only BSP calls sched_tick for global statistics
    if (is_bsp) {
        sched_tick();
    }
}
