// LikeOS-64 PIT Timer Driver
#include "../../include/kernel/timer.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/signal.h"
#include "../../include/kernel/percpu.h"

static volatile uint64_t g_ticks = 0;
static uint32_t g_frequency = 100; // Default 100 Hz
static uint64_t g_boot_epoch = 0;  // Unix epoch seconds at boot (from CMOS RTC)

/* ======================================================================
 * CMOS RTC reader — reads real-time clock to get wall-clock boot time
 * ====================================================================== */
#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int cmos_is_updating(void) {
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (val & 0x0F) + ((val >> 4) * 10);
}

/* Days in each month (non-leap) */
static const uint16_t days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int is_leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Convert date/time to Unix epoch seconds */
static uint64_t datetime_to_epoch(int year, int month, int day,
                                  int hour, int min, int sec)
{
    uint64_t total_days = 0;
    /* Days from 1970 to year-1 */
    for (int y = 1970; y < year; y++) {
        total_days += is_leap_year(y) ? 366 : 365;
    }
    /* Days in months of current year */
    for (int m = 1; m < month; m++) {
        total_days += days_in_month[m - 1];
        if (m == 2 && is_leap_year(year))
            total_days++;
    }
    total_days += (day - 1);
    return total_days * 86400ULL + hour * 3600ULL + min * 60ULL + sec;
}

/* Read CMOS RTC and compute boot epoch */
static void rtc_read_boot_time(void) {
    /* Wait for RTC to not be in update cycle */
    while (cmos_is_updating())
        ;

    uint8_t sec   = cmos_read(0x00);
    uint8_t min   = cmos_read(0x02);
    uint8_t hour  = cmos_read(0x04);
    uint8_t day   = cmos_read(0x07);
    uint8_t month = cmos_read(0x08);
    uint8_t year  = cmos_read(0x09);
    uint8_t century = cmos_read(0x32);  /* Register 0x32 on most ACPI systems */
    uint8_t regB  = cmos_read(0x0B);

    /* Read a second time and keep retrying until two consecutive reads match
     * (avoids reading while the RTC is rolling over) */
    uint8_t s2, m2, h2, d2, mo2, y2, c2;
    do {
        sec   = cmos_read(0x00);
        min   = cmos_read(0x02);
        hour  = cmos_read(0x04);
        day   = cmos_read(0x07);
        month = cmos_read(0x08);
        year  = cmos_read(0x09);
        century = cmos_read(0x32);

        while (cmos_is_updating())
            ;

        s2  = cmos_read(0x00);
        m2  = cmos_read(0x02);
        h2  = cmos_read(0x04);
        d2  = cmos_read(0x07);
        mo2 = cmos_read(0x08);
        y2  = cmos_read(0x09);
        c2  = cmos_read(0x32);
    } while (sec != s2 || min != m2 || hour != h2 ||
             day != d2 || month != mo2 || year != y2 || century != c2);

    /* Convert BCD to binary if needed */
    if (!(regB & 0x04)) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = bcd_to_bin(hour & 0x7F); /* mask PM bit */
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    /* Handle 12-hour mode */
    if (!(regB & 0x02) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    /* Compute full year */
    int full_year;
    if (century > 0) {
        full_year = century * 100 + year;
    } else {
        full_year = (year < 70) ? (2000 + year) : (1900 + year);
    }

    g_boot_epoch = datetime_to_epoch(full_year, month, day, hour, min, sec);
    kprintf("RTC: %d-%02d-%02d %02d:%02d:%02d UTC (epoch=%lu)\n",
            full_year, month, day, hour, min, sec, (unsigned long)g_boot_epoch);
}

uint64_t timer_get_epoch(void) {
    return g_boot_epoch + g_ticks / g_frequency;
}

uint32_t timer_get_frequency(void) {
    return g_frequency;
}

/*
 * Calibrate the actual timer frequency by measuring ticks between two
 * CMOS RTC second boundaries.  This corrects for inaccurate LAPIC timer
 * calibration that can happen in virtual machines (e.g. VMware).
 *
 * Must be called AFTER the timer interrupt is running (PIT or LAPIC).
 * Takes ~2 seconds (waits for two RTC second transitions).
 */
void timer_calibrate_frequency(void) {
    /* Wait for the RTC to NOT be updating */
    while (cmos_is_updating())
        ;
    uint8_t prev_sec = cmos_read(0x00);

    /* Wait for the second to change (first boundary) */
    for (;;) {
        while (cmos_is_updating())
            ;
        uint8_t cur_sec = cmos_read(0x00);
        if (cur_sec != prev_sec)
            break;
    }
    uint64_t t0 = g_ticks;

    /* Wait for the second to change again (second boundary = exactly 1s later) */
    uint8_t boundary_sec;
    while (cmos_is_updating())
        ;
    boundary_sec = cmos_read(0x00);
    for (;;) {
        while (cmos_is_updating())
            ;
        uint8_t cur_sec = cmos_read(0x00);
        if (cur_sec != boundary_sec)
            break;
    }
    uint64_t t1 = g_ticks;

    uint64_t measured = t1 - t0;
    if (measured >= 10 && measured <= 10000) {
        /* Sane range — accept the measurement */
        g_frequency = (uint32_t)measured;
        kprintf("Timer: calibrated frequency = %u Hz (was 100 Hz nominal)\n",
                g_frequency);
    } else {
        kprintf("Timer: calibration out of range (%lu ticks/sec), keeping %u Hz\n",
                (unsigned long)measured, g_frequency);
    }
}

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

    // Read CMOS RTC to establish wall-clock boot time
    rtc_read_boot_time();
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
