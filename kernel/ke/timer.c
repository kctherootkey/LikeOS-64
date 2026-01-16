// LikeOS-64 PIT Timer Driver
#include "../../include/kernel/timer.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"

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
    g_ticks++;
    // Notify scheduler of tick (cooperative only increments counter for now)
    sched_tick();
}
