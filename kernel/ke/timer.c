// LikeOS-64 Timer Driver
#include "../../include/kernel/timer.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/signal.h"
#include "../../include/kernel/percpu.h"
#include "../../include/kernel/pagecache.h"
#include "../../include/kernel/acpi.h"
#include "../../include/kernel/lapic.h"

static volatile uint64_t g_ticks = 0;
static uint32_t g_frequency = 100; // Default 100 Hz
static uint64_t g_boot_epoch = 0;  // Unix epoch seconds at boot (from UEFI or CMOS RTC)

/* Flag to indicate boot_epoch was set from bootloader (UEFI GetTime) */
static int g_boot_epoch_from_uefi = 0;

/* Flag to indicate CMOS RTC failed (returned 0xFF, timeout, etc.) */
static int g_cmos_failed = 0;

/* ACPI FADT boot_arch_flags bits (IA-PC Boot Architecture Flags) */
#define ACPI_FADT_LEGACY_DEVICES    (1<<0)  /* System has LPC or ISA bus devices */
#define ACPI_FADT_8042              (1<<1)  /* System has 8042 controller on port 60/64 */
#define ACPI_FADT_NO_VGA            (1<<2)  /* Not safe to probe for VGA hardware */
#define ACPI_FADT_NO_MSI            (1<<3)  /* MSI must not be enabled */
#define ACPI_FADT_NO_ASPM           (1<<4)  /* PCIe ASPM control must not be enabled */
#define ACPI_FADT_NO_CMOS_RTC       (1<<5)  /* No CMOS real-time clock present */

/* ======================================================================
 * CMOS RTC access — mc146818 style
 * ======================================================================
 *
 * On x86 the standard CMOS RTC is accessed through I/O ports 0x70/0x71:
 *   - Write register index to port 0x70
 *   - Read/write data from/to port 0x71
 *
 * The NMI-disable bit (0x80) is NOT set on 64-bit kernels.
 * Alternate ports 0x74/0x75 are NOT used.
 * P2SB / PCR registers are NOT touched for CMOS access.
 *
 * The mc146818 chip has an Update-In-Progress (UIP) bit in register A
 * (bit 7).  When set, the time registers are being updated and reads
 * may return inconsistent data.  UIP is polled with a timeout.
 * ====================================================================== */

/* RTC register indices (mc146818 standard) */
#define RTC_SECONDS       0x00
#define RTC_MINUTES       0x02
#define RTC_HOURS         0x04
#define RTC_DAY_OF_MONTH  0x07
#define RTC_MONTH         0x08
#define RTC_YEAR          0x09
#define RTC_REG_A         0x0A  /* Status Register A */
#define RTC_REG_B         0x0B  /* Status Register B */
#define RTC_REG_C         0x0C  /* Status Register C (read clears) */
#define RTC_REG_D         0x0D  /* Status Register D */
#define RTC_CENTURY       0x32  /* Century register (ACPI) */

/* Register A bits */
#define RTC_UIP           0x80  /* Update In Progress */
#define RTC_DIV_RESET2    0x70  /* Divider reset (stops updates) */

/* Register B bits */
#define RTC_SET           0x80  /* Inhibit updates for clock setting */
#define RTC_DM_BINARY     0x04  /* Data mode: 1=binary, 0=BCD */
#define RTC_24H           0x02  /* 24-hour mode */

/*
 * io_delay — small I/O delay for CMOS RTC timing.
 *
 * Writing to port 0x80 (POST diagnostic port) introduces a ~1-2µs delay.
 * This is required on modern Intel platforms (Alder Lake, etc.) where
 * eSPI timing may cause problems with back-to-back I/O.
 */
static inline void io_delay(void) {
    outb(0x80, 0);
}

/*
 * cmos_read / cmos_write — standard mc146818 I/O port access with delays.
 *
 * Interrupts are disabled during the index-write + data-read/write sequence
 * to prevent any interrupt from corrupting the CMOS index register state.
 */
static uint8_t cmos_read(uint8_t addr) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    
    outb(0x70, addr);
    io_delay();
    uint8_t val = inb(0x71);
    io_delay();
    
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
    return val;
}

static void cmos_write(uint8_t addr, uint8_t val) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    
    outb(0x70, addr);
    io_delay();
    outb(0x71, val);
    io_delay();
    
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
}

/*
 * rtc_read_consistent — Read all RTC registers while UIP is not set.
 *
 * Polls the UIP bit in register A.  When UIP is clear, reads seconds,
 * then all other registers, then verifies UIP is still clear and
 * seconds haven't changed.  This guarantees a consistent snapshot.
 *
 * timeout_ms: maximum time to wait, in milliseconds.
 * Returns 1 on success, 0 on timeout.
 */
static int rtc_read_consistent(int timeout_ms,
    uint8_t *out_sec, uint8_t *out_min, uint8_t *out_hour,
    uint8_t *out_day, uint8_t *out_mon, uint8_t *out_year,
    uint8_t *out_century, uint8_t *out_ctrl)
{
    /*
     * Poll with a 100us delay between checks.
     * We approximate: each loop iteration does a few inb's plus
     * a short spin.  ~10 iterations ≈ 1ms.  For timeout_ms=1000
     * that's ~10000 iterations max.
     */
    int max_loops = timeout_ms * 10;
    for (int i = 0; i < max_loops; i++) {
        uint8_t seconds;

        /* Read seconds first, before checking UIP */
        seconds = cmos_read(RTC_SECONDS);

        /* Check UIP — if set, wait ~100us and retry */
        if (cmos_read(RTC_REG_A) & RTC_UIP) {
            /* Short delay: a few I/O port reads ≈ ~10us each */
            for (int d = 0; d < 10; d++) inb(0x80);
            continue;
        }

        /* Revalidate seconds */
        if (seconds != cmos_read(RTC_SECONDS))
            continue;

        /* UIP is clear, seconds stable — read all registers */
        uint8_t min     = cmos_read(RTC_MINUTES);
        uint8_t hour    = cmos_read(RTC_HOURS);
        uint8_t day     = cmos_read(RTC_DAY_OF_MONTH);
        uint8_t mon     = cmos_read(RTC_MONTH);
        uint8_t year    = cmos_read(RTC_YEAR);
        uint8_t century = cmos_read(RTC_CENTURY);
        uint8_t ctrl    = cmos_read(RTC_REG_B);

        /* Check UIP again — if set, the above values may be garbage */
        if (cmos_read(RTC_REG_A) & RTC_UIP)
            continue;

        /* Final seconds check — NMI might have interrupted us */
        if (seconds != cmos_read(RTC_SECONDS))
            continue;

        /* Success — consistent snapshot */
        *out_sec     = seconds;
        *out_min     = min;
        *out_hour    = hour;
        *out_day     = day;
        *out_mon     = mon;
        *out_year    = year;
        *out_century = century;
        *out_ctrl    = ctrl;
        return 1;
    }
    return 0;  /* timeout */
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

/*
 * Read CMOS RTC and compute boot epoch.
 *
 * Strategy:
 *   1. First try to read from CMOS RTC (primary source)
 *   2. If CMOS fails (returns 0xFF, timeout, etc.), fall back to
 *      the bootloader's UEFI GetTime value (if available)
 */
static void rtc_read_boot_time(void) {
    uint8_t sec, min, hour, day, month, year, century, ctrl;

    /* Check ACPI FADT for NO_CMOS_RTC flag */
    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table(ACPI_SIG_FADT);
    if (fadt) {
        if (fadt->boot_arch_flags & ACPI_FADT_NO_CMOS_RTC) {
            kprintf("RTC: FADT indicates no CMOS RTC\n");
            goto try_uefi_fallback;
        }
    }

    /* Quick probe: if key registers return 0xFF, CMOS is not accessible */
    uint8_t reg_a = cmos_read(RTC_REG_A);
    uint8_t raw_sec = cmos_read(RTC_SECONDS);
    if (reg_a == 0xFF && raw_sec == 0xFF) {
        kprintf("RTC: CMOS not accessible (0xFF)\n");
        goto try_uefi_fallback;
    }

    /* Read all RTC registers with UIP-avoidance (1000ms timeout) */
    if (!rtc_read_consistent(1000, &sec, &min, &hour, &day, &month,
                             &year, &century, &ctrl)) {
        kprintf("RTC: timeout reading CMOS\n");
        goto try_uefi_fallback;
    }

    /* Sanity check: if all time registers return 0xFF, RTC is not responding */
    if (sec == 0xFF && min == 0xFF && hour == 0xFF &&
        day == 0xFF && month == 0xFF && year == 0xFF) {
        kprintf("RTC: CMOS time registers all 0xFF\n");
        goto try_uefi_fallback;
    }

    /* Convert BCD to binary if needed (check RTC_DM_BINARY in ctrl) */
    if (!(ctrl & RTC_DM_BINARY)) {
        sec     = bcd_to_bin(sec);
        min     = bcd_to_bin(min);
        hour    = bcd_to_bin(hour & 0x7F);  /* mask PM bit */
        day     = bcd_to_bin(day);
        month   = bcd_to_bin(month);
        year    = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    /* Handle 12-hour mode */
    if (!(ctrl & RTC_24H) && (hour & 0x80)) {
        hour = ((hour & 0x7F) + 12) % 24;
    }

    /* Compute full year (century from ACPI FADT) */
    int full_year;
    if (century > 19) {
        full_year = century * 100 + year;
    } else if (century > 0) {
        full_year = century * 100 + year;
    } else {
        full_year = (year < 70) ? (2000 + year) : (1900 + year);
    }

    g_boot_epoch = datetime_to_epoch(full_year, month, day, hour, min, sec);
    kprintf("RTC: %d-%02d-%02d %02d:%02d:%02d UTC (epoch=%lu)\n",
            full_year, month, day, hour, min, sec, (unsigned long)g_boot_epoch);
    return;

try_uefi_fallback:
    /* CMOS failed - set the global flag so calibration knows to skip */
    g_cmos_failed = 1;
    
    /* Try UEFI GetTime from bootloader as fallback */
    if (g_boot_epoch_from_uefi && g_boot_epoch != 0) {
        kprintf("RTC: Using UEFI GetTime from bootloader (epoch=%lu)\n",
                (unsigned long)g_boot_epoch);
        return;
    }
    
    /* No fallback available */
    kprintf("RTC: No time source available!\n");
}

uint64_t timer_get_epoch(void) {
    return g_boot_epoch + g_ticks / g_frequency;
}

uint32_t timer_get_frequency(void) {
    return g_frequency;
}

uint64_t timer_get_uptime(void) {
    return g_ticks / g_frequency;
}

uint64_t timer_get_boot_epoch(void) {
    return g_boot_epoch;
}

/*
 * timer_set_boot_epoch - Set the boot epoch from bootloader.
 *
 * Called from init.c with the boot_epoch value from boot_info_t.
 * The bootloader reads the time using UEFI GetTime() before ExitBootServices.
 */
void timer_set_boot_epoch(uint64_t epoch) {
    if (epoch > 0) {
        g_boot_epoch = epoch;
        g_boot_epoch_from_uefi = 1;
        kprintf("Timer: boot epoch from UEFI = %lu\n", (unsigned long)epoch);
    }
}

/* ======================================================================
 * CMOS RTC writer — writes wall-clock time back to the hardware RTC
 * ====================================================================== */

static uint8_t bin_to_bcd(uint8_t val) {
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

/*
 * Write wall-clock time to the CMOS RTC.
 *
 * Procedure:
 *   1. Read RTC_CONTROL (reg B) to determine BCD vs binary
 *   2. Set RTC_SET bit in reg B to inhibit updates
 *   3. Reset divider in reg A to stop time counting
 *   4. Write all time registers
 *   5. Restore reg A and reg B (clears SET, resumes counting)
 */
static void cmos_write_datetime(uint64_t epoch) {
    /* Break epoch into date/time components */
    uint64_t rem = epoch;
    int sec  = (int)(rem % 60); rem /= 60;
    int min  = (int)(rem % 60); rem /= 60;
    int hour = (int)(rem % 24); rem /= 24;

    /* rem is now days since 1970-01-01 */
    int year = 1970;
    for (;;) {
        int ylen = is_leap_year(year) ? 366 : 365;
        if ((int)rem < ylen) break;
        rem -= ylen;
        year++;
    }

    int month = 1;
    for (int m = 0; m < 12; m++) {
        int mlen = days_in_month[m];
        if (m == 1 && is_leap_year(year)) mlen++;
        if ((int)rem < mlen) break;
        rem -= mlen;
        month++;
    }
    int day = (int)rem + 1;

    int century_val = year / 100;
    int yrs = year % 100;

    /* Read control register to check BCD/binary mode */
    uint8_t save_control = cmos_read(RTC_REG_B);

    /* Convert values to BCD if needed (check RTC_DM_BINARY) */
    uint8_t w_sec = (uint8_t)sec;
    uint8_t w_min = (uint8_t)min;
    uint8_t w_hrs = (uint8_t)hour;
    uint8_t w_day = (uint8_t)day;
    uint8_t w_mon = (uint8_t)month;
    uint8_t w_yrs = (uint8_t)yrs;
    uint8_t w_cen = (uint8_t)century_val;

    if (!(save_control & RTC_DM_BINARY)) {
        w_sec = bin_to_bcd(w_sec);
        w_min = bin_to_bcd(w_min);
        w_hrs = bin_to_bcd(w_hrs);
        w_day = bin_to_bcd(w_day);
        w_mon = bin_to_bcd(w_mon);
        w_yrs = bin_to_bcd(w_yrs);
        w_cen = bin_to_bcd(w_cen);
    }

    /* 1. Set RTC_SET bit in control register (inhibit updates) */
    cmos_write(RTC_REG_B, save_control | RTC_SET);

    /* 2. Reset divider in freq_select register (stop time counting) */
    uint8_t save_freq_select = cmos_read(RTC_REG_A);
    cmos_write(RTC_REG_A, (save_freq_select | RTC_DIV_RESET2));

    /* 3. Write all time registers */
    cmos_write(RTC_YEAR, w_yrs);
    cmos_write(RTC_MONTH, w_mon);
    cmos_write(RTC_DAY_OF_MONTH, w_day);
    cmos_write(RTC_HOURS, w_hrs);
    cmos_write(RTC_MINUTES, w_min);
    cmos_write(RTC_SECONDS, w_sec);
    cmos_write(RTC_CENTURY, w_cen);

    /* 4. Restore registers (clears SET bit, resumes counting) */
    cmos_write(RTC_REG_B, save_control);
    cmos_write(RTC_REG_A, save_freq_select);

    kprintf("RTC: set to %d-%02d-%02d %02d:%02d:%02d UTC (epoch=%lu)\n",
            year, month, day, hour, min, sec, (unsigned long)epoch);
}

/*
 * timer_set_time - Set the system time to a specific epoch value
 *
 * This adjusts g_boot_epoch so that timer_get_epoch() returns the
 * requested wall-clock time, and also syncs the hardware CMOS RTC.
 */
void timer_set_time(uint64_t epoch) {
    /* Compute new boot_epoch such that timer_get_epoch() returns the
     * requested wall-clock time:
     *   epoch_now = boot_epoch + ticks/frequency
     *   => boot_epoch = epoch_now - ticks/frequency
     * But the caller gives us the desired epoch_now, so:
     */
    uint64_t uptime = g_ticks / g_frequency;
    g_boot_epoch = epoch - uptime;

    /* Sync the hardware CMOS RTC to the new wall-clock time */
    cmos_write_datetime(epoch);
}

/*
 * rdtsc — Read the Time Stamp Counter.
 */
static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

/*
 * timer_calibrate_tsc — TSC-based fallback calibration.
 *
 * Uses rdtsc to measure exactly 1 second (based on the known TSC frequency
 * from CPUID 0x15), then counts how many timer IRQ ticks occurred.
 * This works even when CMOS RTC is not accessible.
 *
 * Returns 1 on success, 0 if TSC frequency is unknown.
 */
static int timer_calibrate_tsc(void) {
    uint64_t tsc_hz = lapic_get_tsc_freq();
    if (tsc_hz == 0) {
        kprintf("Timer: TSC frequency unknown, cannot calibrate via TSC\n");
        return 0;
    }

    kprintf("Timer: Calibrating via TSC (freq=%lu Hz)...\n", (unsigned long)tsc_hz);

    /* Measure timer ticks over exactly 1 second using TSC */
    uint64_t tsc_one_second = tsc_hz;

    uint64_t t0 = g_ticks;
    uint64_t tsc_start = rdtsc();

    /* Spin until 1 second has elapsed according to TSC */
    while ((rdtsc() - tsc_start) < tsc_one_second)
        __asm__ volatile("pause");

    uint64_t t1 = g_ticks;
    uint64_t measured = t1 - t0;

    if (measured >= 10 && measured <= 10000) {
        g_frequency = (uint32_t)measured;
        kprintf("Timer: TSC-calibrated frequency = %u Hz\n", g_frequency);
        return 1;
    }

    kprintf("Timer: TSC calibration out of range (%lu ticks/sec), keeping %u Hz\n",
            (unsigned long)measured, g_frequency);
    return 0;
}

/*
 * Calibrate the actual timer frequency by measuring ticks over a known
 * time interval.
 *
 * Method 1 (preferred): CMOS RTC second boundaries (takes ~2s)
 * Method 2 (fallback):  TSC-timed 1-second measurement (no CMOS needed)
 *
 * Must be called AFTER the timer interrupt is running (PIT or LAPIC).
 */
void timer_calibrate_frequency(void) {
    /* If rtc_read_boot_time() already determined CMOS is broken,
     * go straight to TSC fallback — no point probing CMOS again. */
    if (g_cmos_failed) {
        kprintf("Timer: CMOS not available, trying TSC calibration\n");
        if (!timer_calibrate_tsc())
            kprintf("Timer: No calibration source available, keeping %u Hz\n",
                    g_frequency);
        return;
    }

    /* Quick probe: if CMOS seconds register returns 0xFF, fall back to TSC */
    uint8_t probe = cmos_read(RTC_SECONDS);
    uint8_t reg_a = cmos_read(RTC_REG_A);
    
    if (probe == 0xFF || reg_a == 0xFF) {
        kprintf("Timer: CMOS not responding, trying TSC calibration\n");
        if (!timer_calibrate_tsc())
            kprintf("Timer: No calibration source available, keeping %u Hz\n",
                    g_frequency);
        return;
    }

    /* ---- Method 1: CMOS RTC second boundaries ---- */

    /* Wait for the RTC to NOT be updating (tick-based timeout: 200 ticks = 2s) */
    uint64_t start_tick = g_ticks;
    while ((cmos_read(RTC_REG_A) & RTC_UIP) && (g_ticks - start_tick < 200))
        ;
    if (g_ticks - start_tick >= 200) {
        kprintf("Timer: CMOS UIP timeout, trying TSC calibration\n");
        timer_calibrate_tsc();
        return;
    }
    
    uint8_t prev_sec = cmos_read(RTC_SECONDS);

    /* Wait for the second to change (first boundary) — tick-based timeout ~3s */
    start_tick = g_ticks;
    for (;;) {
        while ((cmos_read(RTC_REG_A) & RTC_UIP) && (g_ticks - start_tick < 300))
            ;
        uint8_t cur_sec = cmos_read(RTC_SECONDS);
        if (cur_sec != prev_sec)
            break;
        if (g_ticks - start_tick >= 300) {
            kprintf("Timer: calibration timeout waiting for first RTC second boundary\n");
            kprintf("Timer: Falling back to TSC calibration\n");
            timer_calibrate_tsc();
            return;
        }
    }
    uint64_t t0 = g_ticks;

    /* Wait for the second to change again (second boundary = exactly 1s later) */
    uint8_t boundary_sec;
    while ((cmos_read(RTC_REG_A) & RTC_UIP) && (g_ticks - t0 < 200))
        ;
    boundary_sec = cmos_read(RTC_SECONDS);
    start_tick = g_ticks;
    for (;;) {
        while ((cmos_read(RTC_REG_A) & RTC_UIP) && (g_ticks - start_tick < 300))
            ;
        uint8_t cur_sec = cmos_read(RTC_SECONDS);
        if (cur_sec != boundary_sec)
            break;
        if (g_ticks - start_tick >= 300) {
            kprintf("Timer: calibration timeout waiting for second RTC second boundary\n");
            kprintf("Timer: Falling back to TSC calibration\n");
            timer_calibrate_tsc();
            return;
        }
    }
    uint64_t t1 = g_ticks;

    uint64_t measured = t1 - t0;
    if (measured >= 10 && measured <= 10000) {
        /* Sane range — accept the measurement */
        g_frequency = (uint32_t)measured;
        kprintf("Timer: CMOS-calibrated frequency = %u Hz\n", g_frequency);
    } else {
        kprintf("Timer: CMOS calibration out of range (%lu ticks/sec), trying TSC\n",
                (unsigned long)measured);
        timer_calibrate_tsc();
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

    // Read CMOS RTC (or UEFI fallback) to establish wall-clock boot time
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

        // Update load averages every 500 ticks (~5 seconds at 100Hz)
        if ((g_ticks % 500) == 0) {
            sched_calc_load();
        }

        // Page cache: signal periodic dirty writeback
        pagecache_timer_tick(g_ticks);
    }
    
    // Per-CPU: manage this CPU's current task time slice
    task_t* cur = sched_current();
    if (cur) {
        // Accounting: charge a tick to user or system time.
        // Skip idle tasks and the bootstrap task (PID 0) — their CPU time
        // should not be counted.
        // Guard: this_cpu() reads %gs:0 which faults before percpu_init(),
        // so only attempt the idle-task check after SMP is initialized.
        int skip = (cur->id == 0);
        if (!skip && sched_is_smp()) {
            percpu_t* cpu = this_cpu();
            if (cur == cpu->idle_task)
                skip = 1;
        }
        if (!skip) {
            if (cur->privilege == TASK_USER) {
                if (cur->preempt_frame && (cur->preempt_frame->cs & 3) == 3)
                    cur->utime_ticks++;
                else
                    cur->stime_ticks++;
            } else {
                cur->stime_ticks++;
            }
        }

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
    // Use per-CPU timer_ticks (incremented in percpu) to avoid depending
    // on g_ticks which is only updated by BSP.  When g_ticks==0
    // (BSP hasn't started yet), (0 % 50)==0 would fire load balance on
    // EVERY AP timer interrupt, causing massive spinlock contention.
    if (sched_is_smp()) {
        percpu_t* cpu = this_cpu();
        cpu->timer_ticks++;
        if ((cpu->timer_ticks % 50) == 0) {
            sched_load_balance();
        }
    }
    
    // Only BSP calls sched_tick for global statistics
    if (is_bsp) {
        sched_tick();
    }
}
