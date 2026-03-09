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
void     timer_set_boot_epoch(uint64_t epoch); // Set epoch and sync CMOS RTC

#endif // _KERNEL_TIMER_H_
