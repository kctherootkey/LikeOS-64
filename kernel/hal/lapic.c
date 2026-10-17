// LikeOS - Local APIC (LAPIC) Implementation
// Per-CPU interrupt controller, timer, and IPI support
// Supports both xAPIC (MMIO) and x2APIC (MSR) modes
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

// ============================================================================
// LAPIC Base Address
// ============================================================================

// Default LAPIC base address (can be changed via MSR)
#define LAPIC_DEFAULT_BASE 0xFEE00000ULL

// MSR for LAPIC base
#define MSR_APIC_BASE 0x1B
#define MSR_APIC_BASE_ENABLE (1ULL << 11)
#define MSR_APIC_BASE_X2APIC (1ULL << 10) // x2APIC enable bit
#define MSR_APIC_BASE_BSP (1ULL << 8)

// x2APIC MSR base — register MSR = 0x800 + (MMIO_offset >> 4)
#define X2APIC_MSR_BASE 0x800
#define X2APIC_MSR_ICR 0x830 // x2APIC ICR is a single 64-bit MSR

// State: true when the CPU is in x2APIC mode (registers via MSR, not MMIO)
static bool lapic_x2apic_mode = false;

// Virtual address for LAPIC MMIO (xAPIC mode only, via direct map)
static volatile uint32_t *lapic_base = NULL;
static uint64_t lapic_phys_base = LAPIC_DEFAULT_BASE;
static uint64_t lapic_timer_freq = 0; // Ticks per second after calibration
static uint64_t tsc_freq_hz = 0; // TSC frequency in Hz (0 if unknown)
static bool tsc_reliable = false;

#define VMWARE_HYPERVISOR_PORT 0x5658
#define VMWARE_HYPERVISOR_MAGIC 0x564D5868U
#define VMWARE_CMD_GETHZ 45U
#define CPUID_VMWARE_INFO_LEAF 0x40000000U

#define VMWARE_SIG_EBX 0x61774D56U
#define VMWARE_SIG_ECX 0x4D566572U
#define VMWARE_SIG_EDX 0x65726177U

// ============================================================================
// MSR Access
// ============================================================================

static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t low, high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t low = value & 0xFFFFFFFF;
	uint32_t high = value >> 32;
	__asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// ============================================================================
// CPUID
// ============================================================================

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
			 uint32_t *ecx, uint32_t *edx)
{
	__asm__ volatile("cpuid"
			 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
			 : "a"(leaf), "c"(0));
}

static inline uint64_t rdtsc(void)
{
	uint32_t low, high;
	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

static inline bool vmware_hypervisor_present(void)
{
	uint32_t eax, ebx, ecx, edx;

	cpuid(1, &eax, &ebx, &ecx, &edx);
	if ((ecx & (1U << 31)) == 0) {
		return false;
	}

	cpuid(CPUID_VMWARE_INFO_LEAF, &eax, &ebx, &ecx, &edx);
	return ebx == VMWARE_SIG_EBX && ecx == VMWARE_SIG_ECX &&
	       edx == VMWARE_SIG_EDX;
}

/* The local timer's input clock as the hypervisor states it (GETHZ's
 * ECX), in Hz; 0 when it did not say.  The count-down timer runs at this
 * divided by the divide register, so no calibration against the TSC is
 * needed -- or wanted: the TSC is the guest's apparent time, which can lag
 * the timer's own. */
static uint64_t vmware_bus_hz;

static inline uint64_t vmware_get_tsc_freq_hz(void)
{
	uint32_t lo, hi, cmd, port;

	lo = VMWARE_HYPERVISOR_MAGIC;
	hi = 0xFFFFFFFFU;
	cmd = VMWARE_CMD_GETHZ;
	port = VMWARE_HYPERVISOR_PORT;

	__asm__ volatile("inl (%%dx), %%eax"
			 : "+a"(lo), "+b"(hi), "+c"(cmd), "+d"(port)
			 :
			 : "cc", "memory");

	if (hi == 0xFFFFFFFFU) {
		return 0;
	}
	vmware_bus_hz = cmd;

	return ((uint64_t)hi << 32) | lo;
}

/* The host's wall clock in microseconds since the epoch (UTC), through the
 * hypervisor's GETTIMEFULL call (46): real time as the host keeps it, which
 * the guest's own clocks cannot be relied on to match (timer.c keeps the
 * wall clock to it).  0 when not on VMware or the call is refused. */
#define VMWARE_CMD_GETTIMEFULL 46U

uint64_t lapic_host_time_us(void)
{
	uint32_t a = VMWARE_HYPERVISOR_MAGIC, b = 0xFFFFFFFFU,
		 c = VMWARE_CMD_GETTIMEFULL, d = VMWARE_HYPERVISOR_PORT, si = 0;

	if (!vmware_hypervisor_present())
		return 0;
	__asm__ volatile("inl (%%dx), %%eax"
			 : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(si)
			 :
			 : "cc", "memory");
	if (a != VMWARE_HYPERVISOR_MAGIC)
		return 0;
	return ((((uint64_t)si << 32) | d) * 1000000ULL) + b;
}

#define VMWARE_CMD_STOPCATCHUP 24U

/* Tell the hypervisor to stop catching up: whatever backlog of timer
 * interrupts it still holds against the guest is dropped, and apparent
 * time is no longer held back for it (STOPCATCHUP, 24).  For whoever has
 * just set the guest's clock to the host's: that corrected the lag
 * already, and a catch-up on top of it would run the clock ahead.  A no-op
 * where there is no such hypervisor. */
void lapic_host_stop_catchup(void)
{
	uint32_t a = VMWARE_HYPERVISOR_MAGIC, b = 0, c = VMWARE_CMD_STOPCATCHUP,
		 d = VMWARE_HYPERVISOR_PORT;

	if (!vmware_hypervisor_present())
		return;
	__asm__ volatile("inl (%%dx), %%eax"
			 : "+a"(a), "+b"(b), "+c"(c), "+d"(d)
			 :
			 : "cc", "memory");
}

// ============================================================================
// Delay Functions (for IPI timing)
// ============================================================================

// Set once PIT channel 2 has been seen not to count (see below).
static bool pit_stuck = false;

// Microsecond delay using PIT channel 2
static void pit_delay_us(uint32_t us)
{
	// PIT frequency is 1193182 Hz, so 1 tick = ~838 ns
	// We use PIT channel 2 in one-shot mode
	uint32_t ticks = (us * 1193182ULL) / 1000000;
	if (ticks == 0)
		ticks = 1;
	if (ticks > 65535)
		ticks = 65535;

	// Recent Intel platforms (Meteor/Arrow Lake laptops among them)
	// clock-gate the 8254 by default: the counter never reaches terminal
	// count and OUT2 never rises.  Bound the wait by TSC cycles instead:
	// 8400 cycles per tick is at least one tick's 838 ns for any TSC up to
	// 10 GHz, so giving up never makes the delay shorter than asked.
	uint64_t tsc_limit = (uint64_t)ticks * 8400ULL;
	uint64_t tsc_start = rdtsc();

	if (pit_stuck) {
		while ((rdtsc() - tsc_start) < tsc_limit)
			__asm__ volatile("pause");
		return;
	}

	// Reset PIT channel 2 output latch:
	// 1. Disable gate (bit 0 = 0) to stop the counter and reset OUT2 low
	// 2. Then re-enable gate to let it start counting fresh
	uint8_t port61 = inb(0x61);
	outb(0x61, (port61 & 0xFC) | 0x00); // Gate off, speaker off
	outb(0x61, (port61 & 0xFC) | 0x01); // Gate on, speaker off

	// Program PIT channel 2: lobyte/hibyte, mode 0 (interrupt on terminal count)
	outb(0x43, 0xB0);
	outb(0x42, ticks & 0xFF);
	outb(0x42, (ticks >> 8) & 0xFF);

	// Wait for OUT2 to go high (bit 5 of port 0x61)
	while ((inb(0x61) & 0x20) == 0) {
		if ((rdtsc() - tsc_start) >= tsc_limit) {
			pit_stuck = true;
			kprintf("LAPIC: PIT channel 2 does not count; delays fall back to TSC cycles\n");
			return;
		}
		__asm__ volatile("pause" ::: "memory");
	}
}

// Millisecond delay
static void pit_delay_ms(uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i++) {
		pit_delay_us(1000);
	}
}

// TSC frequency from CPUID leaf 0x15 (crystal * numerator / denominator),
// or leaf 0x16's base frequency when the crystal is not enumerated; 0 when
// neither is there.
static uint64_t tsc_freq_from_cpuid(void)
{
	uint32_t eax, ebx, ecx, edx;
	uint32_t denom, numer, crystal;

	cpuid(0, &eax, &ebx, &ecx, &edx);
	if (eax < 0x15)
		return 0;
	cpuid(0x15, &denom, &numer, &crystal, &edx);
	if (denom == 0 || numer == 0)
		return 0;
	if (crystal != 0)
		return ((uint64_t)crystal * numer) / denom;
	if (eax >= 0x16) {
		uint32_t base_mhz;
		cpuid(0x16, &base_mhz, &ebx, &ecx, &edx);
		return (uint64_t)base_mhz * 1000000ULL;
	}
	return 0;
}

// The TSC frequency for delays.  Drivers probed before smp_init() (the
// Intel graphics driver, xHCI) delay before lapic_timer_calibrate() has
// set tsc_freq_hz; on bare metal, CPUID gives the frequency without
// touching the PIT.  Under a hypervisor the PIT works and CPUID 0x15 is
// not always trustworthy, so the PIT stays the fallback there.  Kept out
// of tsc_freq_hz so the timekeeping code's view does not change.
static uint64_t delay_tsc_freq(void)
{
	static bool probed = false;
	static uint64_t early_hz = 0;
	uint32_t eax, ebx, ecx, edx;

	if (tsc_freq_hz > 0)
		return tsc_freq_hz;
	if (!probed) {
		probed = true;
		cpuid(1, &eax, &ebx, &ecx, &edx);
		if (!(ecx & (1U << 31)))
			early_hz = tsc_freq_from_cpuid();
	}
	return early_hz;
}

// TSC-based microsecond delay (exported for use by smp.c etc.)
// Falls back to PIT if TSC frequency is not known.
void lapic_delay_us(uint32_t us)
{
	uint64_t hz = delay_tsc_freq();

	if (hz > 0) {
		uint64_t tsc_target = ((uint64_t)us * hz) / 1000000ULL;
		uint64_t tsc_start = rdtsc();
		while ((rdtsc() - tsc_start) < tsc_target)
			__asm__ volatile("pause");
	} else {
		pit_delay_us(us);
	}
}

void lapic_delay_ms(uint32_t ms)
{
	uint64_t hz = delay_tsc_freq();

	if (hz > 0) {
		uint64_t tsc_target = ((uint64_t)ms * hz) / 1000ULL;
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

static inline volatile uint32_t *get_lapic_base(void)
{
	if (!lapic_base) {
		lapic_base = (volatile uint32_t *)phys_to_virt(lapic_phys_base);
	}
	return lapic_base;
}

uint32_t lapic_read(uint32_t reg)
{
	WARN_ON(reg & 0xF); /* xAPIC registers must be 16-byte aligned */
	if (lapic_x2apic_mode) {
		// x2APIC: register MSR = 0x800 + (MMIO_offset >> 4)
		return (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4));
	}
	return *(volatile uint32_t *)((uint8_t *)get_lapic_base() + reg);
}

void lapic_write(uint32_t reg, uint32_t value)
{
	WARN_ON(reg & 0xF); /* xAPIC registers must be 16-byte aligned */
	if (lapic_x2apic_mode) {
		// x2APIC: register MSR = 0x800 + (MMIO_offset >> 4)
		// NOTE: ICR is handled specially in IPI functions (single 64-bit MSR)
		// DFR (0x0E0) does not exist in x2APIC mode — skip silently
		if (reg == LAPIC_DFR)
			return;
		// LDR (0x0D0) is read-only in x2APIC mode — skip silently
		if (reg == LAPIC_LDR)
			return;
		wrmsr(X2APIC_MSR_BASE + (reg >> 4), value);
		return;
	}
	*(volatile uint32_t *)((uint8_t *)get_lapic_base() + reg) = value;
	// Read back to ensure write is completed (memory fence) — xAPIC only
	(void)*(volatile uint32_t *)((uint8_t *)get_lapic_base() + LAPIC_ID);
}

// ============================================================================
// LAPIC Core Functions
// ============================================================================

void lapic_early_detect(void)
{
	uint64_t msr = rdmsr(MSR_APIC_BASE);
	if (msr & MSR_APIC_BASE_X2APIC)
		lapic_x2apic_mode = true;
	lapic_phys_base = msr & 0xFFFFFFFFFFFFF000ULL;
}

bool lapic_is_available(void)
{
	uint32_t eax, ebx, ecx, edx;
	cpuid(1, &eax, &ebx, &ecx, &edx);
	return (edx & (1 << 9)) != 0; // APIC feature bit
}

// Get initial APIC ID from CPUID (safe to call before lapic_init)
// This doesn't require LAPIC MMIO access, so it works even if LAPIC isn't fully enabled
__no_stack_protector uint32_t lapic_get_id_cpuid(void)
{
	uint32_t eax, ebx, ecx, edx;
	cpuid(1, &eax, &ebx, &ecx, &edx);
	return (ebx >> 24) & 0xFF; // Initial APIC ID is in EBX[31:24]
}

uint64_t lapic_get_base(void)
{
	uint64_t msr = rdmsr(MSR_APIC_BASE);
	return msr & 0xFFFFFFFFFFFFF000ULL;
}

uint32_t lapic_get_id(void)
{
	if (lapic_x2apic_mode) {
		// x2APIC: full 32-bit APIC ID from MSR 0x802
		return (uint32_t)rdmsr(X2APIC_MSR_BASE + (LAPIC_ID >> 4));
	}
	return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void)
{
	lapic_write(LAPIC_EOI, 0);
}

void lapic_enable(void)
{
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
	uint32_t svr_old = svr;
	svr |= LAPIC_SVR_ENABLE;
	svr = (svr & 0xFFFFFF00) | LAPIC_SPURIOUS_VECTOR;
	// Clear Suppress EOI Broadcast (bit 12) — UEFI firmware on x2APIC
	// platforms often sets this.  With it enabled, LAPIC EOI never clears
	// IOAPIC Remote IRR, so level-triggered interrupts (ACPI SCI) fire
	// exactly once and then stop.  Clearing it restores normal EOI
	// broadcast to IOAPIC for proper level-triggered operation.
	svr &= ~LAPIC_SVR_SUPPRESS_EOI_BC;
	if (svr_old & LAPIC_SVR_SUPPRESS_EOI_BC)
		kprintf("LAPIC: cleared Suppress EOI Broadcast (SVR 0x%x -> 0x%x)\n",
			svr_old, svr);
	lapic_write(LAPIC_SVR, svr);
}

void lapic_disable(void)
{
	// Disable via SVR
	uint32_t svr = lapic_read(LAPIC_SVR);
	svr &= ~LAPIC_SVR_ENABLE;
	lapic_write(LAPIC_SVR, svr);
}

void lapic_setup_logical_dest(uint32_t logical_id)
{
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
	if (logical_id > 7)
		logical_id = logical_id & 7;
	lapic_write(LAPIC_LDR, (1u << logical_id) << 24);
}

void lapic_init(void)
{
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

	smp_dbg("LAPIC: Base address = 0x%lx, x2APIC = %s\n", lapic_phys_base,
		lapic_x2apic_mode ? "yes" : "no");

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
		lapic_write(LAPIC_LVT_LINT1,
			    0x00000400); // NMI delivery mode (100)
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

void lapic_timer_calibrate(void)
{
	uint32_t eax, ebx, ecx, edx;

	if (vmware_hypervisor_present()) {
		uint64_t vmware_tsc_freq = vmware_get_tsc_freq_hz();
		if (vmware_tsc_freq > 0) {
			tsc_freq_hz = vmware_tsc_freq;
			tsc_reliable = true;
			smp_dbg("LAPIC: VMware hypervisor TSC freq=%lu Hz\n",
				vmware_tsc_freq);

			lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
			lapic_write(LAPIC_LVT_TIMER,
				    LAPIC_LVT_MASKED | LAPIC_TIMER_ONESHOT);
			lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

			uint64_t tsc_start = rdtsc();
			uint64_t tsc_target = vmware_tsc_freq / 10;
			while ((rdtsc() - tsc_start) < tsc_target)
				__asm__ volatile("pause");

			uint32_t elapsed =
				0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
			lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

			lapic_timer_freq = (uint64_t)elapsed * 10;
			/* The hypervisor's own figure wins over the measurement:
			 * both clocks of the measurement are virtual, and the
			 * TSC is the one that can fall behind. */
			if (vmware_bus_hz >= 1000000ULL)
				lapic_timer_freq = vmware_bus_hz / 16;
			smp_dbg("LAPIC: VMware TSC-calibrated timer_freq=%lu Hz\n",
				lapic_timer_freq);

			if (lapic_timer_freq >= 100000 &&
			    lapic_timer_freq <= 500000000) {
				return;
			}

			smp_dbg("LAPIC: VMware TSC calibration out of range, falling back\n");
		}
	}

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
					crystal =
						(uint32_t)(((uint64_t)base_mhz *
							    1000000ULL *
							    denom) /
							   numer);
				}
			}

			if (crystal > 0) {
				// We know TSC frequency: tsc_freq = crystal * numer / denom
				uint64_t tsc_freq =
					((uint64_t)crystal * numer) / denom;

				// Store TSC frequency globally for TSC-based delay functions
				tsc_freq_hz = tsc_freq;

				smp_dbg("LAPIC: CPUID 0x15: crystal=%u Hz, TSC freq=%lu Hz\n",
					crystal, tsc_freq);

				if (tsc_freq > 0) {
					// Use TSC to time exactly 100ms of LAPIC timer counting
					uint64_t tsc_target =
						tsc_freq /
						10; // 100ms worth of TSC ticks

					lapic_write(LAPIC_TIMER_DCR,
						    LAPIC_TIMER_DIV_16);
					lapic_write(
						LAPIC_LVT_TIMER,
						LAPIC_LVT_MASKED |
							LAPIC_TIMER_ONESHOT);
					lapic_write(LAPIC_TIMER_ICR,
						    0xFFFFFFFF);

					uint64_t tsc_start = rdtsc();
					while ((rdtsc() - tsc_start) <
					       tsc_target)
						__asm__ volatile("pause");

					uint32_t elapsed =
						0xFFFFFFFF -
						lapic_read(LAPIC_TIMER_CCR);
					lapic_write(LAPIC_LVT_TIMER,
						    LAPIC_LVT_MASKED);

					lapic_timer_freq =
						(uint64_t)elapsed * 10;

					smp_dbg("LAPIC: TSC-calibrated: elapsed=%u in 100ms, timer_freq=%lu Hz\n",
						elapsed, lapic_timer_freq);

					if (lapic_timer_freq >= 100000 &&
					    lapic_timer_freq <= 500000000) {
						return; // Good calibration
					}
					smp_dbg("LAPIC: TSC-based result out of range, falling back to PIT\n");
				}
			}
		}
	}

	// =========================================================================
	// Method 2 (fallback): PIT channel 2 direct measurement
	// Used when CPUID 0x15 is not available (e.g., QEMU, older CPUs, VMware)
	// Also measure TSC frequency by reading rdtsc around the PIT delay.
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

/* ---- the tick and the high-resolution timers: the local timer, one-shot --
 *
 * A periodic timer is a promise of one interrupt per period, and a
 * hypervisor keeps that promise even when it could not: when the virtual
 * machine was not running for a while it delivers every period it owes
 * afterwards, several times faster than real time, until it has caught up.
 * Measured on VMware right after boot: 2600-3800 timer interrupts per CPU
 * in five seconds instead of 1000, gaps of up to 240 ms in between, for
 * some twenty seconds -- and everything timed by the tick stuttered.
 *
 * So the local timer runs one-shot and is armed for whichever comes first:
 * the CPU's next tick or the earliest high-resolution timer on its queue
 * (hrtimer.c keeps one per CPU).  It is the CPU's one event source.  Two
 * ways to arm it:
 *
 *  - TSC deadline (CPUID.1:ECX[24]): the timer fires when the TSC reaches
 *    a value written to IA32_TSC_DEADLINE.  The tick is kept on a grid of
 *    TSC boundaries; when it fires, the next one is the first boundary after
 *    now, so periods that went by while the machine was stopped are skipped,
 *    not replayed.  Deadline and clock are the same counter, so they cannot
 *    disagree.  Used wherever the CPU offers it and its microcode is known
 *    good (lapic_tsc_deadline_usable).
 *
 *  - The count-down timer in one-shot mode, everywhere else.  Here the timer
 *    and the TSC are two different clocks, and on a hypervisor they do not
 *    run at the same rate: the count-down follows real time, the TSC follows
 *    the guest's "apparent time", which falls behind when the machine is not
 *    run.  Converting TSC distances into counts therefore makes the timer
 *    fire before the TSC has reached the point it was armed for; re-armed for
 *    the remainder it fires early again, thousands of times a second, and
 *    every one of those interrupts is a trip through the hypervisor that
 *    puts the guest further behind.  Measured on VMware: 2550 local timer
 *    interrupts a second on one CPU, the TSC advancing 99 ms per real
 *    second, the boot stalled for a minute and a half.
 *
 *    So in this mode the timer keeps its own time.  Every load of the
 *    initial-count register and every read of the current count is added up
 *    into a per-CPU count-down clock (g_lc_now), and the tick is a fixed
 *    number of counts on that clock -- one period of the timer's own time,
 *    never derived from the TSC.  A stall costs the ticks it covered; none is
 *    replayed, and nothing can loop.  Timekeeping does not depend on it: the
 *    tick resynchronises the tick count to the clock (timer.c).
 *
 *    A high-resolution timer's deadline is on the clock, so its distance has
 *    to be converted after all.  If the timer comes back with that deadline
 *    still ahead, it is re-armed for the remainder once; coming back early a
 *    second time for the same deadline means the clocks disagree, and that
 *    deadline is left to the tick instead of chased.  At most two extra
 *    interrupts per timer, however far apart the clocks are.
 *
 * Periodic mode is left only for a machine whose TSC frequency is unknown. */
#ifndef LAPIC_TICK_TSC_DEADLINE
#define LAPIC_TICK_TSC_DEADLINE 1
#endif

#define MSR_IA32_TSC_DEADLINE 0x6E0
#define MSR_IA32_UCODE_REV 0x8B

/* The fewest counts the count-down is ever loaded with: below this the
 * load and the expiry overlap. */
#define LC_MIN_COUNTS 16

enum lapic_tick_mode {
	TICK_PERIODIC = 0,
	TICK_TSC_DEADLINE,
	TICK_ONESHOT,
};

static int g_tick_mode = TICK_PERIODIC;
static uint64_t g_tick_period_tsc;
static uint32_t g_tick_hz; /* the rate the tick was started with */
/* Per CPU: the local timer is running as this CPU's event source. */
static volatile int g_ce_running[MAX_CPUS];

/* TSC-deadline mode: the next tick, a TSC value. */
static uint64_t g_tick_next_tsc[MAX_CPUS];

/* Count-down mode: the timer's own clock, in counts (see above), what the
 * register was last loaded with and has not been accounted for yet, and the
 * tick on that clock. */
static uint64_t g_lc_period;
static uint64_t g_lc_now[MAX_CPUS];
static uint32_t g_lc_armed[MAX_CPUS];
static uint64_t g_lc_tick_next[MAX_CPUS];
/* The high-resolution deadline the current load is for (0: the tick), how
 * often it came back early, and a deadline given up on (left to the tick). */
static uint64_t g_hrt_target[MAX_CPUS];
static uint32_t g_hrt_early[MAX_CPUS];
static uint64_t g_hrt_gaveup[MAX_CPUS];

/* Processors whose TSC-deadline timer misbehaves below a microcode
 * revision: family 6 model, stepping range, first good revision (0: every
 * revision of those steppings is bad). */
static const struct {
	uint8_t model, step_lo, step_hi;
	uint32_t min_rev;
} g_deadline_ucode[] = {
	{ 0x3F, 0x2, 0x2, 0x3a }, /* Haswell server EP */
	{ 0x3F, 0x4, 0x4, 0x0f }, /* Haswell server EX */
	{ 0x4F, 0x0, 0xf, 0x0b000020 }, /* Broadwell server */
	{ 0x56, 0x2, 0x2, 0x00000011 }, /* Broadwell-DE */
	{ 0x56, 0x3, 0x3, 0x0700000e },
	{ 0x56, 0x4, 0x4, 0x0f00000c },
	{ 0x56, 0x5, 0x5, 0x0e000003 },
	{ 0x55, 0x3, 0x3, 0x01000136 }, /* Skylake server */
	{ 0x55, 0x4, 0x4, 0x02000014 },
	{ 0x55, 0x5, 0xf, 0 },
	{ 0x3C, 0x0, 0xf, 0x22 }, /* Haswell */
	{ 0x45, 0x0, 0xf, 0x20 }, /* Haswell mobile */
	{ 0x46, 0x0, 0xf, 0x17 }, /* Haswell with eDRAM */
	{ 0x3D, 0x0, 0xf, 0x25 }, /* Broadwell */
	{ 0x47, 0x0, 0xf, 0x17 }, /* Broadwell with eDRAM */
	{ 0x4E, 0x0, 0xf, 0xb2 }, /* Skylake mobile */
	{ 0x5E, 0x0, 0xf, 0xb2 }, /* Skylake desktop / H */
	{ 0x8E, 0x0, 0xf, 0x52 }, /* Kaby Lake mobile */
	{ 0x9E, 0x0, 0xf, 0x52 }, /* Kaby Lake desktop / H */
};

static uint32_t lapic_ucode_rev(void)
{
	uint32_t eax, ebx, ecx, edx;

	/* The revision is latched into the MSR by CPUID(1) after it has been
	 * cleared. */
	wrmsr(MSR_IA32_UCODE_REV, 0);
	cpuid(1, &eax, &ebx, &ecx, &edx);
	return (uint32_t)(rdmsr(MSR_IA32_UCODE_REV) >> 32);
}

/* May the TSC-deadline timer be used?  When the CPU offers it, the TSC
 * frequency is known, and -- on real Intel hardware -- the microcode is at
 * least the revision that fixed it on this model.  A hypervisor's deadline
 * timer is its own and is taken as offered. */
static bool lapic_tsc_deadline_usable(void)
{
	uint32_t eax, ebx, ecx, edx, family, model, stepping, rev;
	uint32_t vendor[3];

	if (!LAPIC_TICK_TSC_DEADLINE || tsc_freq_hz == 0)
		return false;
	cpuid(1, &eax, &ebx, &ecx, &edx);
	if (!(ecx & (1U << 24)))
		return false;
	if (ecx & (1U << 31))
		return true; /* a hypervisor's */

	/* "GenuineIntel": EBX, EDX, ECX */
	cpuid(0, &eax, &vendor[0], &vendor[2], &vendor[1]);
	if (kmemcmp(vendor, "GenuineIntel", 12) != 0)
		return true;
	cpuid(1, &eax, &ebx, &ecx, &edx);
	family = (eax >> 8) & 0xF;
	model = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);
	stepping = eax & 0xF;
	if (family != 6)
		return true;
	for (size_t i = 0; i < sizeof(g_deadline_ucode) / sizeof(g_deadline_ucode[0]); i++) {
		if (g_deadline_ucode[i].model != model ||
		    stepping < g_deadline_ucode[i].step_lo ||
		    stepping > g_deadline_ucode[i].step_hi)
			continue;
		rev = lapic_ucode_rev();
		if (g_deadline_ucode[i].min_rev == 0 ||
		    rev < g_deadline_ucode[i].min_rev) {
			kprintf("LAPIC: TSC-deadline timer disabled: microcode revision 0x%x on model 0x%x stepping %u (needs 0x%x)\n",
				rev, model, stepping, g_deadline_ucode[i].min_rev);
			return false;
		}
		return true;
	}
	return true;
}

/* This CPU's slot.  Callers run after the per-CPU areas exist. */
static uint32_t lapic_tick_slot(void)
{
	uint32_t c = this_cpu_id();

	return c < MAX_CPUS ? c : 0;
}

/* Is this CPU's local timer running one-shot (the clock-event device)? */
bool lapic_clockevent_ready(uint32_t cpu)
{
	return g_tick_mode != TICK_PERIODIC && cpu < MAX_CPUS &&
	       g_ce_running[cpu];
}

/* ---- TSC-deadline mode ---- */

/* The TSC value at which the nanosecond deadline `ns' falls, or 0 when it
 * lies beyond the next tick anyway (the tick then comes first). */
static uint64_t lapic_ns_to_tsc_deadline(uint64_t ns, uint64_t now_tsc)
{
	uint64_t now_ns = timer_get_precise_ns();
	uint64_t d;

	if (ns <= now_ns)
		return now_tsc;
	d = ns - now_ns;
	if (d > 1000000000ULL)
		return 0;
	return now_tsc + d * (tsc_freq_hz / 1000ULL) / 1000000ULL;
}

static void tscdl_program(uint32_t c, uint64_t hrt_ns)
{
	uint64_t now = rdtsc(), next = g_tick_next_tsc[c];

	if (hrt_ns) {
		uint64_t h = lapic_ns_to_tsc_deadline(hrt_ns, now);

		if (h && (int64_t)(h - next) < 0)
			next = h;
	}
	wrmsr(MSR_IA32_TSC_DEADLINE, next);
}

static int tscdl_interrupt(uint32_t c, uint64_t hnext)
{
	uint64_t now = rdtsc(), next = g_tick_next_tsc[c];
	uint64_t p = g_tick_period_tsc;

	if ((int64_t)(now - next) < 0) {
		tscdl_program(c, hnext);
		return 0; /* a high-resolution timer's turn */
	}
	/* The first boundary after now: whatever lies between was missed and
	 * is not owed. */
	g_tick_next_tsc[c] = next + ((now - next) / p + 1) * p;
	tscdl_program(c, hnext);
	return 1;
}

/* ---- count-down mode ---- */

/* Account for what has run off the counter since it was last loaded or
 * looked at.  Interrupts off. */
static void lc_sync(uint32_t c)
{
	uint32_t armed = g_lc_armed[c], ccr;

	if (!armed)
		return;
	ccr = lapic_read(LAPIC_TIMER_CCR);
	if (ccr > armed)
		ccr = armed;
	g_lc_now[c] += armed - ccr;
	g_lc_armed[c] = ccr;
}

/* Load the counter for the earlier of the tick and `hrt_ns'. */
static void lc_program(uint32_t c, uint64_t hrt_ns)
{
	uint64_t cnt, d, now_ns, hc;

	lc_sync(c);
	cnt = g_lc_tick_next[c] > g_lc_now[c] ?
		      g_lc_tick_next[c] - g_lc_now[c] :
		      1;
	g_hrt_target[c] = 0;
	if (hrt_ns && hrt_ns != g_hrt_gaveup[c]) {
		now_ns = timer_get_precise_ns();
		d = hrt_ns > now_ns ? hrt_ns - now_ns : 0;
		if (d < 1000000000ULL) {
			hc = d * lapic_timer_freq / 1000000000ULL;
			if (hc < LC_MIN_COUNTS)
				hc = LC_MIN_COUNTS;
			if (hc < cnt) {
				cnt = hc;
				g_hrt_target[c] = hrt_ns;
			}
		}
	}
	if (cnt < LC_MIN_COUNTS && g_hrt_target[c])
		cnt = LC_MIN_COUNTS;
	if (cnt == 0)
		cnt = 1;
	if (cnt > 0xFFFFFFFFULL)
		cnt = 0xFFFFFFFFULL;
	g_lc_armed[c] = (uint32_t)cnt;
	lapic_write(LAPIC_TIMER_ICR, (uint32_t)cnt);
}

static int lc_interrupt(uint32_t c, uint64_t hnext)
{
	uint64_t armed_for = g_hrt_target[c];
	int tick = 0;

	/* What ran off the counter -- all of the load when it expired, less
	 * when the interrupt was already pending as the counter was loaded
	 * again (a timer armed with interrupts off): then only the new load's
	 * elapsed part counts, and the rest of it is still running. */
	lc_sync(c);
	if (g_lc_now[c] >= g_lc_tick_next[c]) {
		tick = 1;
		g_lc_tick_next[c] += g_lc_period;
		if (g_lc_tick_next[c] <= g_lc_now[c])
			g_lc_tick_next[c] = g_lc_now[c] + g_lc_period;
	}
	/* Armed for a high-resolution deadline that is still the queue's head
	 * after the queue was run: it came back early. */
	if (armed_for && hnext == armed_for) {
		if (++g_hrt_early[c] >= 2) {
			g_hrt_gaveup[c] = armed_for; /* the tick will do */
			g_hrt_early[c] = 0;
		}
	} else {
		g_hrt_early[c] = 0;
	}
	lc_program(c, hnext);
	return tick;
}

/* ---- both ---- */

/* Arm this CPU's local timer for the earlier of its next tick and
 * `hrtimer_next_ns' (0: no high-resolution timer queued here).  Interrupts
 * off, on the CPU concerned. */
void lapic_clockevent_program(uint64_t hrtimer_next_ns)
{
	uint32_t c;

	if (g_tick_mode == TICK_PERIODIC)
		return;
	c = lapic_tick_slot();
	if (!g_ce_running[c])
		return; /* not running on this CPU yet */
	if (g_tick_mode == TICK_TSC_DEADLINE) {
		tscdl_program(c, hrtimer_next_ns);
		return;
	}
	if (hrtimer_next_ns != g_hrt_target[c])
		g_hrt_early[c] = 0;
	lc_program(c, hrtimer_next_ns);
}

/* The local timer's interrupt.  Runs this CPU's expired high-resolution
 * timers, advances the tick when it is due, and arms the timer for the
 * next event.  Returns 1 when the tick is due (the caller then does the
 * tick's work), 0 when only high-resolution timers were.  In periodic mode,
 * and before this CPU's timer runs one-shot, every interrupt is a tick. */
int lapic_timer_interrupt(void)
{
	uint32_t c;
	uint64_t hnext;

	if (g_tick_mode == TICK_PERIODIC)
		return 1;
	c = lapic_tick_slot();
	if (!g_ce_running[c])
		return 1; /* not this CPU's one-shot timer: the legacy tick */
	hnext = hrtimer_run_local();
	if (g_tick_mode == TICK_TSC_DEADLINE)
		return tscdl_interrupt(c, hnext);
	return lc_interrupt(c, hnext);
}

void lapic_timer_start(uint32_t frequency)
{
	uint32_t c;

	if (lapic_timer_freq == 0) {
		lapic_timer_calibrate();
	}

	if (frequency && tsc_freq_hz) {
		c = lapic_tick_slot();
		g_tick_hz = frequency;
		g_tick_period_tsc = tsc_freq_hz / frequency;
		if (!g_tick_period_tsc)
			g_tick_period_tsc = 1;
		if (lapic_tsc_deadline_usable()) {
			lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR |
							     LAPIC_TIMER_TSC_DEADLINE);
			/* The mode switch must reach the timer before the
			 * deadline does, or the write to the MSR is ignored. */
			__asm__ volatile("mfence" ::: "memory");
			g_tick_next_tsc[c] = rdtsc() + g_tick_period_tsc;
			g_tick_mode = TICK_TSC_DEADLINE;
			g_ce_running[c] = 1;
			wrmsr(MSR_IA32_TSC_DEADLINE, g_tick_next_tsc[c]);
			hrtimer_clockevent_started();
			smp_dbg("LAPIC: tick at %u Hz on TSC deadlines (period %llu cycles)\n",
				frequency, (unsigned long long)g_tick_period_tsc);
			return;
		}
		g_lc_period = lapic_timer_freq / frequency;
		if (!g_lc_period)
			g_lc_period = 1;
		lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
		lapic_write(LAPIC_LVT_TIMER,
			    LAPIC_TIMER_VECTOR | LAPIC_TIMER_ONESHOT);
		g_lc_now[c] = 0;
		g_lc_armed[c] = 0;
		g_lc_tick_next[c] = g_lc_period;
		g_hrt_target[c] = 0;
		g_hrt_early[c] = 0;
		g_hrt_gaveup[c] = 0;
		g_tick_mode = TICK_ONESHOT;
		g_ce_running[c] = 1;
		lc_program(c, 0);
		hrtimer_clockevent_started();
		smp_dbg("LAPIC: tick at %u Hz, one-shot count-down (period %llu counts)\n",
			frequency, (unsigned long long)g_lc_period);
		return;
	}

	// Calculate initial count for desired frequency
	uint32_t count = (uint32_t)(lapic_timer_freq / frequency);
	if (count == 0)
		count = 1;

	// Set up timer in periodic mode
	lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
	lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
	lapic_write(LAPIC_TIMER_ICR, count);

	smp_dbg("LAPIC: Timer started at %u Hz (count=%u)\n", frequency, count);
}

void lapic_timer_stop(void)
{
	uint32_t c = lapic_tick_slot();

	g_ce_running[c] = 0;
	lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
	lapic_write(LAPIC_TIMER_ICR, 0);
	g_lc_armed[c] = 0;
	if (g_tick_mode == TICK_TSC_DEADLINE)
		wrmsr(MSR_IA32_TSC_DEADLINE, 0);
}

uint64_t lapic_timer_get_frequency(void)
{
	return lapic_timer_freq;
}

uint64_t lapic_get_tsc_freq(void)
{
	return tsc_freq_hz;
}

bool lapic_tsc_is_reliable(void)
{
	return tsc_reliable;
}

void lapic_set_tsc_freq(uint64_t hz)
{
	tsc_freq_hz = hz;
	/* A tick on TSC deadlines is a number of TSC cycles: keep it the same
	 * length of time. */
	if (g_tick_hz && hz)
		g_tick_period_tsc = hz / g_tick_hz;
}

// ============================================================================
// IPI Functions
// ============================================================================

void lapic_ipi_wait(void)
{
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
static inline void x2apic_write_icr(uint32_t dest, uint32_t cmd)
{
	uint64_t val = ((uint64_t)dest << 32) | cmd;
	wrmsr(X2APIC_MSR_ICR, val);
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector)
{
	if (lapic_x2apic_mode) {
		x2apic_write_icr(apic_id,
				 vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
					 LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
		return;
	}
	// xAPIC: Set destination in high ICR
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);

	// Send IPI: fixed delivery, physical destination, edge triggered
	lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED |
					   LAPIC_ICR_PHYSICAL |
					   LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);

	lapic_ipi_wait();
}

/* NMI delivery: no vector — the receiving CPU dispatches to its INT 2
 * handler (IST2) regardless of its current IF flag.  Used to probe a CPU
 * that has stopped acknowledging fixed-delivery IPIs. */
void lapic_send_nmi(uint32_t apic_id)
{
	if (lapic_x2apic_mode) {
		x2apic_write_icr(apic_id, LAPIC_ICR_NMI | LAPIC_ICR_PHYSICAL |
						  LAPIC_ICR_ASSERT |
						  LAPIC_ICR_EDGE);
		return;
	}
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
	lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_NMI | LAPIC_ICR_PHYSICAL |
					   LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);
	lapic_ipi_wait();
}

void lapic_send_init(uint32_t apic_id)
{
	if (lapic_x2apic_mode) {
		// INIT assert
		x2apic_write_icr(apic_id, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
						  LAPIC_ICR_ASSERT |
						  LAPIC_ICR_LEVEL);
		lapic_delay_ms(10);
		// x2APIC: INIT de-assert is not needed (only used by old P6/P4 xAPIC)
		return;
	}
	// xAPIC
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
	lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
					   LAPIC_ICR_ASSERT | LAPIC_ICR_LEVEL);

	lapic_ipi_wait();
	lapic_delay_ms(10);

	// Deassert (level triggered) — xAPIC only
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
	lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_INIT | LAPIC_ICR_PHYSICAL |
					   LAPIC_ICR_DEASSERT |
					   LAPIC_ICR_LEVEL);

	lapic_ipi_wait();
}

void lapic_send_sipi(uint32_t apic_id, uint8_t vector)
{
	if (lapic_x2apic_mode) {
		x2apic_write_icr(apic_id, vector | LAPIC_ICR_STARTUP |
						  LAPIC_ICR_PHYSICAL |
						  LAPIC_ICR_ASSERT |
						  LAPIC_ICR_EDGE);
		return;
	}
	// xAPIC
	lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
	lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_STARTUP |
					   LAPIC_ICR_PHYSICAL |
					   LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE);

	lapic_ipi_wait();
}

void lapic_send_ipi_all_excl_self(uint32_t vector)
{
	if (lapic_x2apic_mode) {
		x2apic_write_icr(0, vector | LAPIC_ICR_FIXED |
					    LAPIC_ICR_PHYSICAL |
					    LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE |
					    LAPIC_ICR_ALL_EXCL_SELF);
		return;
	}
	lapic_write(LAPIC_ICR_HIGH, 0);
	lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED |
					   LAPIC_ICR_PHYSICAL |
					   LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE |
					   LAPIC_ICR_ALL_EXCL_SELF);
	lapic_ipi_wait();
}

void lapic_send_ipi_all_incl_self(uint32_t vector)
{
	if (lapic_x2apic_mode) {
		x2apic_write_icr(0, vector | LAPIC_ICR_FIXED |
					    LAPIC_ICR_PHYSICAL |
					    LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE |
					    LAPIC_ICR_ALL_INCL_SELF);
		return;
	}
	lapic_write(LAPIC_ICR_HIGH, 0);
	lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED |
					   LAPIC_ICR_PHYSICAL |
					   LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE |
					   LAPIC_ICR_ALL_INCL_SELF);
	lapic_ipi_wait();
}

void lapic_send_ipi_self(uint32_t vector)
{
	if (lapic_x2apic_mode) {
		x2apic_write_icr(0, vector | LAPIC_ICR_FIXED |
					    LAPIC_ICR_PHYSICAL |
					    LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE |
					    LAPIC_ICR_SELF);
		return;
	}
	lapic_write(LAPIC_ICR_HIGH, 0);
	lapic_write(LAPIC_ICR_LOW,
		    vector | LAPIC_ICR_FIXED | LAPIC_ICR_PHYSICAL |
			    LAPIC_ICR_ASSERT | LAPIC_ICR_EDGE | LAPIC_ICR_SELF);
	lapic_ipi_wait();
}
