// LikeOS-64 CPU P-state / HWP configuration
//
// Requests maximum-performance frequency behavior so the core does not linger
// in a low idle P-state.  On real hardware (measured on a Lenovo: core at 51%
// of base frequency even at boot) the firmware governor ramps erratically,
// producing "output starts slow then suddenly speeds up" and command timings
// that vary 3x run-to-run.  A balanced (idle-low) policy still leaves visible
// ramp gaps, so we pin the energy-performance preference to maximum.
// Hypervisors expose a fixed vCPU clock and never show this.
//
// Two mechanisms, newest first:
//   1. HWP (Skylake 2015+): enable IA32_PM_ENABLE, set IA32_HWP_REQUEST with
//      EPP=0 (max performance).  The hardware picks the frequency up to the
//      advertised highest, ramping hard under load.
//   2. Legacy Enhanced SpeedStep (pre-HWP Intel): enable EIST in
//      IA32_MISC_ENABLE and request the max ratio via IA32_PERF_CTL (write an
//      over-range ratio the hardware clamps to its own maximum -- so we never
//      have to read IA32_PLATFORM_INFO, which #GPs on some virtual CPUs).
//
// The legacy path is gated on GenuineIntel + EIST so hypervisors (which expose
// neither HWP nor, usually, EIST) and AMD take neither path and we never touch
// an MSR that would fault.  There is no #GP fixup handler in this kernel, so
// every rdmsr/wrmsr here must be one the CPUID feature bits guarantee exists.

#include <kernel/hal/cpu_pstate.h>
#include <kernel/io/console.h>

// Intel thermal/power MSRs
#define MSR_IA32_MISC_ENABLE 0x1A0
#define MSR_IA32_PERF_CTL 0x199
#define MSR_IA32_ENERGY_PERF_BIAS 0x1B0
#define MSR_IA32_PM_ENABLE 0x770
#define MSR_IA32_HWP_CAPABILITIES 0x771
#define MSR_IA32_HWP_REQUEST 0x774

#define MISC_ENABLE_EIST (1ULL << 16) // IA32_MISC_ENABLE: Enhanced SpeedStep enable
#define PERF_CTL_TURBO_DISENGAGE (1ULL << 32) // IA32_PERF_CTL: 1 = disable turbo

static inline uint64_t pstate_rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void pstate_wrmsr(uint32_t msr, uint64_t val)
{
	__asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)val),
			 "d"((uint32_t)(val >> 32)));
}

// True if CPUID vendor string is "GenuineIntel".
static int cpu_is_intel(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
	return b == 0x756E6547 /* "Genu" */ && d == 0x49656E69 /* "ineI" */ &&
	       c == 0x6C65746E /* "ntel" */;
}

void cpu_pstate_init(int verbose)
{
	uint32_t a, b, c, d;

	// CPUID leaf 6 (thermal and power management)
	__asm__ volatile("cpuid"
			 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			 : "a"(6), "c"(0));
	(void)b;
	(void)d;
	int has_hwp = (a >> 7) & 1; // EAX[7]  = HWP
	int has_epp = (a >> 10) & 1; // EAX[10] = HWP Energy-Performance Preference
	int has_epb = (c >> 3) & 1; // ECX[3]  = IA32_ENERGY_PERF_BIAS

	// -- Modern path: Hardware-managed P-states ------------------------------
	if (has_hwp) {
		// Enable HWP for this logical processor (a one-way latch on real HW).
		pstate_wrmsr(MSR_IA32_PM_ENABLE, 1);

		uint64_t cap = pstate_rdmsr(MSR_IA32_HWP_CAPABILITIES);
		uint8_t highest = (uint8_t)(cap & 0xFF); // max (turbo) perf level
		uint8_t guaranteed =
			(uint8_t)((cap >> 8) & 0xFF); // base (P1) perf level

		// IA32_HWP_REQUEST: [7:0]=Minimum [15:8]=Maximum [23:16]=Desired
		// [31:24]=EPP.  Minimum=guaranteed keeps a high floor, Maximum=highest
		// allows turbo, Desired=0 lets the HW pick up to Maximum, EPP=0 (bits
		// stay clear) biases hard toward performance -> immediate ramp.
		uint64_t req = (uint64_t)guaranteed | ((uint64_t)highest << 8);
		pstate_wrmsr(MSR_IA32_HWP_REQUEST, req);

		// If EPP isn't wired into HWP_REQUEST, the legacy EPB knob sets bias.
		if (!has_epp && has_epb)
			pstate_wrmsr(MSR_IA32_ENERGY_PERF_BIAS, 0);

		if (verbose)
			kprintf("cpu_pstate: HWP enabled, EPP=0 max-perf (perf %u..%u, EPP %s)\n",
				guaranteed, highest, has_epp ? "native" : "via EPB");
		return;
	}

	// -- Legacy path: Enhanced SpeedStep on GenuineIntel ---------------------
	// CPUID.01H:ECX[7] = EIST.  Gate on Intel + EIST so we only touch
	// IA32_MISC_ENABLE / IA32_PERF_CTL where they are guaranteed to exist.
	__asm__ volatile("cpuid"
			 : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			 : "a"(1), "c"(0));
	int has_eist = (c >> 7) & 1;

	if (cpu_is_intel() && has_eist) {
		// Enable EIST if the firmware left it off (may be locked; then this
		// write is silently ignored, which is fine).
		uint64_t misc = pstate_rdmsr(MSR_IA32_MISC_ENABLE);
		if (!(misc & MISC_ENABLE_EIST))
			pstate_wrmsr(MSR_IA32_MISC_ENABLE,
				     misc | MISC_ENABLE_EIST);

		// Request the maximum ratio.  Writing an over-range ratio (0xFF in
		// bits [15:8]) makes the hardware clamp to its own max, so we never
		// need IA32_PLATFORM_INFO.  Clear the turbo-disengage bit so turbo
		// stays available.
		uint64_t pctl = pstate_rdmsr(MSR_IA32_PERF_CTL);
		pctl = (pctl & ~0xFFFFULL & ~PERF_CTL_TURBO_DISENGAGE) | 0xFF00ULL;
		pstate_wrmsr(MSR_IA32_PERF_CTL, pctl);

		if (has_epb)
			pstate_wrmsr(MSR_IA32_ENERGY_PERF_BIAS, 0);

		if (verbose)
			kprintf("cpu_pstate: EIST, PERF_CTL requested max ratio%s\n",
				has_epb ? " (EPB=performance)" : "");
		return;
	}

	// -- No controllable P-states (hypervisor fixed clock, AMD, or older) ----
	// Bias to performance via EPB when available; otherwise nothing to do.
	if (has_epb)
		pstate_wrmsr(MSR_IA32_ENERGY_PERF_BIAS, 0);
	if (verbose)
		kprintf("cpu_pstate: no HWP/EIST%s\n",
			has_epb ? " (EPB set to performance)" :
				  " (nothing to tune)");
}
