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
void timer_irq_handler(void);

#endif // _KERNEL_TIMER_H_
