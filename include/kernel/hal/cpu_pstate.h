// LikeOS-64 CPU P-state / HWP configuration
#ifndef _KERNEL_HAL_CPU_PSTATE_H_
#define _KERNEL_HAL_CPU_PSTATE_H_

/* Configure THIS logical CPU for maximum-performance frequency behavior:
 * enable HWP (Hardware-managed P-states) and request EPP=0 (Energy-Performance
 * Preference = max performance) so the core ramps to a high frequency
 * immediately under load instead of lingering in a low idle P-state.
 *
 * On real laptops the default firmware governor idles the core low and ramps
 * erratically, which shows up as "output starts slow then suddenly speeds up"
 * and 3x-varying command timings; a balanced (idle-low) policy still leaves
 * visible ramp gaps, so this pins the preference to performance.  The hardware
 * still enforces thermal limits, so this changes the frequency *preference*,
 * not a hard pin.
 *
 * Must be called once per CPU (the BSP and every AP) because IA32_HWP_REQUEST
 * is a per-logical-processor MSR.  verbose!=0 prints a one-line summary (pass
 * non-zero only on the BSP so the APs stay quiet). */
void cpu_pstate_init(int verbose);

#endif // _KERNEL_HAL_CPU_PSTATE_H_
