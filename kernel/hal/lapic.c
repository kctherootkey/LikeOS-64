// LikeOS-64 - Local APIC (LAPIC) Implementation
// Per-CPU interrupt controller, timer, and IPI support

#include "../../include/kernel/lapic.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/interrupt.h"

// ============================================================================
// LAPIC Base Address
// ============================================================================

// Default LAPIC base address (can be changed via MSR)
#define LAPIC_DEFAULT_BASE      0xFEE00000ULL

// MSR for LAPIC base
#define MSR_APIC_BASE           0x1B
#define MSR_APIC_BASE_ENABLE    (1ULL << 11)
#define MSR_APIC_BASE_BSP       (1ULL << 8)

// Virtual address for LAPIC MMIO (via direct map)
static volatile uint32_t* lapic_base = NULL;
static uint64_t lapic_phys_base = LAPIC_DEFAULT_BASE;
static uint64_t lapic_timer_freq = 0;  // Ticks per second after calibration

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
    
    // Set up PIT channel 2 for one-shot mode
    outb(0x61, (inb(0x61) & 0xFD) | 0x01);  // Enable speaker gate, disable speaker
    outb(0x43, 0xB0);  // Channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count)
    outb(0x42, ticks & 0xFF);
    outb(0x42, (ticks >> 8) & 0xFF);
    
    // Wait for output to go high (bit 5 of port 0x61)
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

// ============================================================================
// LAPIC Register Access
// ============================================================================

static inline volatile uint32_t* get_lapic_base(void) {
    if (!lapic_base) {
        lapic_base = (volatile uint32_t*)phys_to_virt(lapic_phys_base);
    }
    return lapic_base;
}

uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t*)((uint8_t*)get_lapic_base() + reg);
}

void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)((uint8_t*)get_lapic_base() + reg) = value;
    // Read back to ensure write is completed (memory fence)
    (void)lapic_read(LAPIC_ID);
}

// ============================================================================
// LAPIC Core Functions
// ============================================================================

bool lapic_is_available(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;  // APIC feature bit
}

uint64_t lapic_get_base(void) {
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    return msr & 0xFFFFFFFFFFFFF000ULL;
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

void lapic_enable(void) {
    // Enable LAPIC via MSR
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    msr |= MSR_APIC_BASE_ENABLE;
    wrmsr(MSR_APIC_BASE, msr);
    
    // Update our base address from MSR
    lapic_phys_base = msr & 0xFFFFFFFFFFFFF000ULL;
    lapic_base = NULL;  // Force re-mapping
    
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
    // Set destination format to flat model
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    
    // Set logical destination ID (use bit position based on CPU number)
    lapic_write(LAPIC_LDR, (1 << logical_id) << 24);
}

void lapic_init(void) {
    if (!lapic_is_available()) {
        kprintf("LAPIC: Not available on this CPU\n");
        return;
    }
    
    // Get base address from MSR
    lapic_phys_base = lapic_get_base();
    lapic_base = NULL;  // Will be set on first access
    
    kprintf("LAPIC: Base address = 0x%lx\n", lapic_phys_base);
    
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
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_ERROR_VECTOR);
    
    // Clear error status (write twice as per Intel manual)
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    
    // Clear any pending interrupts
    lapic_eoi();
    
    kprintf("LAPIC: Initialized (APIC ID = %u)\n", apic_id);
}

// ============================================================================
// LAPIC Timer Functions
// ============================================================================

void lapic_timer_calibrate(void) {
    // Use PIT to calibrate LAPIC timer
    // We'll count LAPIC ticks over a known PIT interval
    
    kprintf("LAPIC: Calibrating timer...\n");
    
    // Set up LAPIC timer with divide by 16
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    
    // Set initial count to max and start timer in one-shot mode
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_ONESHOT);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
    
    // Wait 10ms using PIT
    pit_delay_ms(10);
    
    // Read current count
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
    
    // Stop timer
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    
    // Calculate frequency (ticks per second)
    // elapsed ticks / 10ms = elapsed * 100 ticks per second
    lapic_timer_freq = (uint64_t)elapsed * 100;
    
    kprintf("LAPIC: Timer frequency = %lu Hz (elapsed=%u in 10ms)\n", 
            lapic_timer_freq, elapsed);
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
    
    kprintf("LAPIC: Timer started at %u Hz (count=%u)\n", frequency, count);
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0);
}

uint64_t lapic_timer_get_frequency(void) {
    return lapic_timer_freq;
}

// ============================================================================
// IPI Functions
// ============================================================================

void lapic_ipi_wait(void) {
    // Wait for delivery to complete (check delivery status bit)
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_PENDING) {
        __asm__ volatile("pause" ::: "memory");
    }
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector) {
    // Set destination in high ICR
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send IPI: fixed delivery, physical destination, edge triggered
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
    
    lapic_ipi_wait();
}

void lapic_send_init(uint32_t apic_id) {
    // Set destination
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send INIT IPI: level triggered, assert
    lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL);
    
    lapic_ipi_wait();
    
    // Wait 10ms
    pit_delay_ms(10);
    
    // Deassert (level triggered)
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_DEASSERT | LAPIC_ICR_LEVEL);
    
    lapic_ipi_wait();
}

void lapic_send_sipi(uint32_t apic_id, uint8_t vector) {
    // Set destination
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    
    // Send SIPI: vector is the page number of startup code (address >> 12)
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_STARTUP | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
    
    lapic_ipi_wait();
}

void lapic_send_ipi_all_excl_self(uint32_t vector) {
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_ALL_EXCL_SELF);
    lapic_ipi_wait();
}

void lapic_send_ipi_all_incl_self(uint32_t vector) {
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_ALL_INCL_SELF);
    lapic_ipi_wait();
}

void lapic_send_ipi_self(uint32_t vector) {
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
                LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_SELF);
    lapic_ipi_wait();
}
