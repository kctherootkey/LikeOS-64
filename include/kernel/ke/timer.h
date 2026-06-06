// LikeOS-64 PIT Timer Driver
#ifndef _KERNEL_TIMER_H_
#define _KERNEL_TIMER_H_

#include "types.h"

// PIT ports
#define PIT_CHANNEL0_DATA   0x40
#define PIT_CHANNEL1_DATA   0x41
#define PIT_CHANNEL2_DATA   0x42
#define PIT_CMD             0x43

// PIT frequency (1.193182 MHz base oscillator)
#define PIT_BASE_FREQ       1193182

// Timer API
void timer_init(uint32_t frequency_hz);
void timer_start(void);
void timer_stop(void);
uint64_t timer_ticks(void);
uint64_t timer_get_epoch(void);
uint32_t timer_get_frequency(void);
void timer_calibrate_frequency(void);
void timer_irq_handler(void);
uint64_t timer_get_uptime(void);      // seconds since boot
uint64_t timer_get_boot_epoch(void);  // Unix epoch at boot time
void     timer_set_boot_epoch(uint64_t epoch); // Set boot epoch from UEFI bootloader
void     timer_set_time(uint64_t epoch);       // Set wall-clock time (adjusts epoch, syncs CMOS)
uint64_t timer_get_tsc_at_tick(void);          // (legacy, returns 0)
uint64_t timer_get_ticks_at_cpu_tick(void);    // Same as timer_ticks()
uint64_t timer_get_precise_us(void);           // Microseconds since boot (tick + PM Timer)
void     timer_init_hpet(void);                // Probe and enable HPET for precise timing
void     timer_init_pmtimer(void);             // Probe and enable ACPI PM Timer
uint32_t timer_pmtimer_read_raw(void);         // Raw PM Timer counter value
uint64_t timer_pmtimer_delta_us(uint32_t t0, uint32_t t1); // Microseconds between two raw PM Timer snapshots

// Inline rdtsc — nanosecond-resolution, works across CPUs on VMware
static inline uint64_t timer_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif // _KERNEL_TIMER_H_
