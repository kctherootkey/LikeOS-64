// LikeOS-64 - Local APIC (LAPIC) Implementation
// Per-CPU interrupt controller, timer, and IPI support
// Supports both xAPIC (MMIO) and x2APIC (MSR) modes

#include "../../include/kernel/lapic.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/smp.h"

// ============================================================================
// LAPIC Base Address
// ============================================================================

// Default LAPIC base address (can be changed via MSR)
#define LAPIC_DEFAULT_BASE      0xFEE00000ULL

// MSR for LAPIC base
#define MSR_APIC_BASE           0x1B
#define MSR_APIC_BASE_ENABLE    (1ULL << 11)
#define MSR_APIC_BASE_X2APIC    (1ULL << 10)   // x2APIC enable bit
#define MSR_APIC_BASE_BSP       (1ULL << 8)

// x2APIC MSR base — register MSR = 0x800 + (MMIO_offset >> 4)
#define X2APIC_MSR_BASE         0x800
#define X2APIC_MSR_ICR          0x830   // x2APIC ICR is a single 64-bit MSR

// State: true when the CPU is in x2APIC mode (registers via MSR, not MMIO)
static bool lapic_x2apic_mode = false;

// Virtual address for LAPIC MMIO (xAPIC mode only, via direct map)
static volatile uint32_t* lapic_base = NULL;
static uint64_t lapic_phys_base = LAPIC_DEFAULT_BASE;
static uint64_t lapic_timer_freq = 0;  // Ticks per second after calibration
static uint64_t tsc_freq_hz = 0;      // TSC frequency in Hz (0 if unknown)

// ============================================================================
// MSR Access
// ============================================================================

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// ============================================================================
// CPUID
// ============================================================================

static inline void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0));
}

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// ============================================================================
// Delay Functions (for IPI timing)
// ============================================================================

// Microsecond delay using PIT channel 2
static void pit_delay_us(uint32_t us) {
    // PIT frequency is 1193182 Hz, so 1 tick = ~838 ns
    // We use PIT channel 2 in one-shot mode
    uint32_t ticks = (us * 1193182ULL) / 1000000;
    if (ticks == 0) ticks = 1;
    if (ticks > 65535) ticks = 65535;
    
    // Reset PIT channel 2 output latch:
    // 1. Disable gate (bit 0 = 0) to stop the counter and reset OUT2 low
    // 2. Then re-enable gate to let it start counting fresh
    uint8_t port61 = inb(0x61);
    outb(0x61, (port61 & 0xFC) | 0x00);  // Gate off, speaker off
    outb(0x61, (port61 & 0xFC) | 0x01);  // Gate on, speaker off
    
    // Program PIT channel 2: lobyte/hibyte, mode 0 (interrupt on terminal count)
    outb(0x43, 0xB0);
    outb(0x42, ticks & 0xFF);
    outb(0x42, (ticks >> 8) & 0xFF);
    
    // Wait for OUT2 to go high (bit 5 of port 0x61)
    while ((inb(0x61) & 0x20) == 0) {
        __asm__ volatile("pause" ::: "memory");
    }
}

// Millisecond delay
static void pit_delay_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        pit_delay_us(1000);
    }
}

// TSC-based microsecond delay (exported for use by smp.c etc.)
// Falls back to PIT if TSC frequency is not known.
void lapic_delay_us(uint32_t us) {
    if (tsc_freq_hz > 0) {
        uint64_t tsc_target = ((uint64_t)us * tsc_freq_hz) / 1000000ULL;
        uint64_t tsc_start = rdtsc();
        while ((rdtsc() - tsc_start) < tsc_target)
            __asm__ volatile("pause");
    } else {
        pit_delay_us(us);
    }
}

void lapic_delay_ms(uint32_t ms) {
    if (tsc_freq_hz > 0) {
        uint64_t tsc_target = ((uint64_t)ms * tsc_freq_hz) / 1000ULL;
        uint64_t tsc_start = rdtsc();
        while ((rdtsc() - tsc_start) < tsc_target)
            __asm__ volatile("pause");
    } else {
        pit_delay_ms(ms);
    }
}

// ============================================================================
// LAPIC Register Access (xAPIC MMIO or x2APIC MSR)
// ============================================================================

static inline volatile uint32_t* get_lapic_base(void) {
    if (!lapic_base) {
        lapic_base = (volatile uint32_t*)phys_to_virt(lapic_phys_base);
    }
    return lapic_base;
}

uint32_t lapic_read(uint32_t reg) {
    if (lapic_x2apic_mode) {
        // x2APIC: register MSR = 0x800 + (MMIO_offset >> 4)
        return (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4));
    }
    return *(volatile uint32_t*)((uint8_t*)get_lapic_base() + reg);
}

void lapic_write(uint32_t reg, uint32_t value) {
    if (lapic_x2apic_mode) {
        // x2APIC: register MSR = 0x800 + (MMIO_offset >> 4)
        // NOTE: ICR is handled specially in IPI functions (single 64-bit MSR)
        // DFR (0x0E0) does not exist in x2APIC mode — skip silently
        if (reg == LAPIC_DFR) return;
        // LDR (0x0D0) is read-only in x2APIC mode — skip silently
        if (reg == LAPIC_LDR) return;
        wrmsr(X2APIC_MSR_BASE + (reg >> 4), value);
        return;
    }
    *(volatile uint32_t*)((uint8_t*)get_lapic_base() + reg) = value;
    // Read back to ensure write is completed (memory fence) — xAPIC only
    (void)*(volatile uint32_t*)((uint8_t*)get_lapic_base() + LAPIC_ID);
}

// ============================================================================
// LAPIC Core Functions
// ============================================================================

void lapic_early_detect(void) {
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    if (msr & MSR_APIC_BASE_X2APIC)
        lapic_x2apic_mode = true;
    lapic_phys_base = msr & 0xFFFFFFFFFFFFF000ULL;
}

bool lapic_is_available(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;  // APIC feature bit
}

// Get initial APIC ID from CPUID (safe to call before lapic_init)
// This doesn't require LAPIC MMIO access, so it works even if LAPIC isn't fully enabled
uint32_t lapic_get_id_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ebx >> 24) & 0xFF;  // Initial APIC ID is in EBX[31:24]
}

uint64_t lapic_get_base(void) {
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    return msr & 0xFFFFFFFFFFFFF000ULL;
}

uint32_t lapic_get_id(void) {
    if (lapic_x2apic_mode) {
        // x2APIC: full 32-bit APIC ID from MSR 0x802
        return (uint32_t)rdmsr(X2APIC_MSR_BASE + (LAPIC_ID >> 4));
    }
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

void lapic_enable(void) {
    // Read current APIC MSR
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    
    // Check if firmware already enabled x2APIC mode
    if (msr & MSR_APIC_BASE_X2APIC) {
        lapic_x2apic_mode = true;
        smp_dbg("LAPIC: x2APIC mode detected (firmware-enabled)\n");
    } else {
        lapic_x2apic_mode = false;
    }
    
    // Ensure APIC is globally enabled
    msr |= MSR_APIC_BASE_ENABLE;
    // Preserve x2APIC bit if already set (cannot go back to xAPIC without reset)
    wrmsr(MSR_APIC_BASE, msr);
    
    // Update our base address from MSR
    uint64_t new_phys = msr & 0xFFFFFFFFFFFFF000ULL;
    if (new_phys != lapic_phys_base) {
        lapic_phys_base = new_phys;
        lapic_base = NULL;
    }
    
    // Enable LAPIC and set spurious vector
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE;
    svr = (svr & 0xFFFFFF00) | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);
}

void lapic_disable(void) {
    // Disable via SVR
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr &= ~LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_SVR, svr);
}

void lapic_setup_logical_dest(uint32_t logical_id) {
    if (lapic_x2apic_mode) {
        // x2APIC: LDR is read-only, hardware auto-assigns logical ID.
        // DFR does not exist. Nothing to do.
        return;
    }
    // xAPIC: Set destination format to flat model
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    
    // Set logical destination ID (use bit position based on CPU number)
    // Flat model only supports 8 logical destinations (bits 24-31 of LDR).
    // Cap to 7 to avoid undefined shift behavior with large APIC IDs.
    if (logical_id > 7) logical_id = logical_id & 7;
    lapic_write(LAPIC_LDR, (1u << logical_id) << 24);
}

void lapic_init(void) {
    if (!lapic_is_available()) {
        smp_dbg("LAPIC: Not available on this CPU\n");
        return;
    }
    
    // Detect x2APIC mode from MSR before any register access
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    if (msr & MSR_APIC_BASE_X2APIC) {
        lapic_x2apic_mode = true;
    }
    
    // Get base address from MSR (for xAPIC MMIO)
    uint64_t phys = msr & 0xFFFFFFFFFFFFF000ULL;
    if (phys != lapic_phys_base) {
        lapic_phys_base = phys;
        lapic_base = NULL;
    }
    
    smp_dbg("LAPIC: Base address = 0x%lx, x2APIC = %s\n",
            lapic_phys_base, lapic_x2apic_mode ? "yes" : "no");
    
    // Enable LAPIC
    lapic_enable();
    
    // Set task priority to accept all interrupts
    lapic_write(LAPIC_TPR, 0);
    
    // Set up logical destination for this CPU
    uint32_t apic_id = lapic_get_id();
    lapic_setup_logical_dest(apic_id);
    
    // Mask all LVT entries initially
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PMC, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_ERROR_VECTOR);
    
    // Configure LINT0 and LINT1 based on BSP vs AP
    uint64_t apic_msr = rdmsr(MSR_APIC_BASE);
    if (apic_msr & MSR_APIC_BASE_BSP) {
        // BSP: Do NOT touch LINT0/LINT1!
        // UEFI firmware already configured virtual wire mode (LINT0=ExtINT)
        // for PIC interrupt pass-through. Overwriting it can break PIC delivery.
    } else {
        // APs: mask LINT0 (only BSP receives PIC interrupts)
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
        // APs: LINT1 = NMI
        lapic_write(LAPIC_LVT_LINT1, 0x00000400);  // NMI delivery mode (100)
    }
    
    // Clear error status (write twice as per Intel manual)
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    
    // Clear any pending interrupts
    lapic_eoi();
    
    // Notify the interrupt subsystem that LAPIC is now active on BSP
    // so that pic_send_eoi() also sends LAPIC EOI
    if (apic_msr & MSR_APIC_BASE_BSP) {
        interrupts_set_lapic_active(1);
    }
    
    smp_dbg("LAPIC: Initialized (APIC ID = %u)\n", apic_id);
}

// ============================================================================
// LAPIC Timer Functions
// ============================================================================

void lapic_timer_calibrate(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // =========================================================================
    // Method 1: CPUID leaf 0x15 — TSC frequency → TSC-timed LAPIC calibration
    //
    // CPUID 0x15: EAX = denominator, EBX = numerator, ECX = crystal freq (Hz)
    // TSC_freq = crystal * numerator / denominator
    //
    // If crystal (ECX) == 0, derive it from CPUID 0x16 (base MHz).
    //
    // Once we know the TSC frequency, we use rdtsc to time exactly 100ms
    // while the LAPIC timer counts down.  This completely bypasses the PIT.
    // =========================================================================
    cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;

    if (max_leaf >= 0x15) {
        uint32_t denom, numer, crystal;
        cpuid(0x15, &denom, &numer, &crystal, &edx);

        if (denom != 0 && numer != 0) {
            // Try to get crystal frequency
            if (crystal == 0 && max_leaf >= 0x16) {
                uint32_t base_mhz;
                cpuid(0x16, &base_mhz, &ebx, &ecx, &edx);
                if (base_mhz > 0) {
                    // TSC_freq = base_mhz * 1e6, crystal = TSC_freq * denom / numer
                    crystal = (uint32_t)(((uint64_t)base_mhz * 1000000ULL * denom) / numer);
                }
            }

            if (crystal > 0) {
                // We know TSC frequency: tsc_freq = crystal * numer / denom
                uint64_t tsc_freq = ((uint64_t)crystal * numer) / denom;
                
                // Store TSC frequency globally for TSC-based delay functions
                tsc_freq_hz = tsc_freq;
                
                smp_dbg("LAPIC: CPUID 0x15: crystal=%u Hz, TSC freq=%lu Hz\n",
                        crystal, tsc_freq);
                
                if (tsc_freq > 0) {
                    // Use TSC to time exactly 100ms of LAPIC timer counting
                    uint64_t tsc_target = tsc_freq / 10;  // 100ms worth of TSC ticks
                    
                    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
                    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_ONESHOT);
                    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
                    
                    uint64_t tsc_start = rdtsc();
                    while ((rdtsc() - tsc_start) < tsc_target)
                        __asm__ volatile("pause");
                    
                    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
                    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
                    
                    lapic_timer_freq = (uint64_t)elapsed * 10;
                    
                    smp_dbg("LAPIC: TSC-calibrated: elapsed=%u in 100ms, timer_freq=%lu Hz\n",
                            elapsed, lapic_timer_freq);
                    
                    if (lapic_timer_freq >= 100000 && lapic_timer_freq <= 500000000) {
                        return;  // Good calibration
                    }
                    smp_dbg("LAPIC: TSC-based result out of range, falling back to PIT\n");
                }
            }
        }
    }

    // =========================================================================
    // Method 2 (fallback): PIT channel 2 direct measurement
    // Used when CPUID 0x15 is not available (e.g., QEMU, older CPUs)
    // =========================================================================
    smp_dbg("LAPIC: Calibrating timer via PIT ch2 (100ms)...\n");
    
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_ONESHOT);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
    
    pit_delay_ms(100);
    
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    
    lapic_timer_freq = (uint64_t)elapsed * 10;
    
    smp_dbg("LAPIC: PIT-calibrated: timer_freq=%lu Hz (elapsed=%u in 100ms)\n", 
            lapic_timer_freq, elapsed);
    
    if (lapic_timer_freq < 100000) {
        smp_dbg("LAPIC: WARNING: calibration failed, using 1 MHz fallback\n");
        lapic_timer_freq = 1000000;
    }
}

void lapic_timer_start(uint32_t frequency) {
    if (lapic_timer_freq == 0) {
        lapic_timer_calibrate();
    }
    
    // Calculate initial count for desired frequency
    uint32_t count = (uint32_t)(lapic_timer_freq / frequency);
    if (count == 0) count = 1;
    
    // Set up timer in periodic mode
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, count);
    
    smp_dbg("LAPIC: Timer started at %u Hz (count=%u)\n", frequency, count);
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0);
}

uint64_t lapic_timer_get_frequency(void) {
    return lapic_timer_freq;
}

uint64_t lapic_get_tsc_freq(void) {
    return tsc_freq_hz;
}

// ============================================================================
// IPI Functions
// ============================================================================

void lapic_ipi_wait(void) {
    if (lapic_x2apic_mode) {
        // x2APIC: ICR write is synchronous, no need to poll delivery status
        return;
    }
    // xAPIC: Wait for delivery to complete (check delivery status bit)
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_PENDING) {
        __asm__ volatile("pause" ::: "memory");
    }
}

// x2APIC helper: write the single 64-bit ICR MSR (0x830)
// dest is the full 32-bit APIC ID in bits 32-63, command in bits 0-31
static inline void x2apic_write_icr(uint32_t dest, uint32_t cmd) {
    uint64_t val = ((uint64_t)dest << 32) | cmd;
    wrmsr(X2APIC_MSR_ICR, val);
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    if (lapic_x2apic_mode) {
        x2apic_write_icr(apic_id, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                          LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
        return;
    }
    // xAPIC: Set destination in high ICR
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send IPI: fixed delivery, physical destination, edge triggered
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
    
    lapic_ipi_wait();
}

void lapic_send_init(uint32_t apic_id) {
    if (lapic_x2apic_mode) {
        // INIT assert
        x2apic_write_icr(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
                          LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL);
        pit_delay_ms(10);
        // x2APIC: INIT de-assert is not needed (only used by old P6/P4 xAPIC)
        return;
    }
    // xAPIC
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL);
    
    lapic_ipi_wait();
    pit_delay_ms(10);
    
    // Deassert (level triggered) — xAPIC only
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_DEASSERT | LAPIC_ICR_LEVEL);
    
    lapic_ipi_wait();
}

void lapic_send_sipi(uint32_t apic_id, uint8_t vector) {
    if (lapic_x2apic_mode) {
        x2apic_write_icr(apic_id, vector | LAPIC_ICR_STARTUP | LAPIC_ICR_PHYSICAL |
                          LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
        return;
    }
    // xAPIC
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_STARTUP | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
    
    lapic_ipi_wait();
}

void lapic_send_ipi_all_excl_self(uint32_t vector) {
    if (lapic_x2apic_mode) {
        x2apic_write_icr(0, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                          LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_ALL_EXCL_SELF);
        return;
    }
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_ALL_EXCL_SELF);
    lapic_ipi_wait();
}

void lapic_send_ipi_all_incl_self(uint32_t vector) {
    if (lapic_x2apic_mode) {
        x2apic_write_icr(0, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                          LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_ALL_INCL_SELF);
        return;
    }
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_ALL_INCL_SELF);
    lapic_ipi_wait();
}

void lapic_send_ipi_self(uint32_t vector) {
    if (lapic_x2apic_mode) {
        x2apic_write_icr(0, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                          LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_SELF);
        return;
    }
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_SELF);
    lapic_ipi_wait();
}
