// Stack canary support for the UEFI bootloader
#include <stdint.h>

uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

// Randomize __stack_chk_guard early in boot using RDTSC + function address.
// Marked noinline so the function has a stable address usable for entropy.
// Avoids address-of-local so GCC doesn't instrument it with stack protection
// (which would cause a false canary mismatch when we write the new value).
__attribute__((noinline, no_stack_protector))
void boot_init_canary(void) {
    uint64_t canary = 0;

    /* Primary: RDRAND with CPUID guard (avoids #UD on CPUs without it). */
    {
        uint32_t ecx = 0;
        __asm__ volatile("cpuid" : "=c"(ecx) : "a"(1) : "ebx", "edx");
        if (ecx & (1U << 30)) {
            for (int i = 0; i < 10; i++) {
                uint8_t cf = 0;
                __asm__ volatile(
                    "rdrand %0\n\t"
                    "setc   %1\n\t"
                    : "=r"(canary), "=qm"(cf) : : "cc");
                if (cf) goto apply_mask;
            }
        }
    }

    /* Fallback: RDTSC + function address (no &local — avoids instrumentation). */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        canary  = ((uint64_t)hi << 32) | lo;
        canary ^= (uint64_t)(uintptr_t)boot_init_canary;
    }

apply_mask:
    if (canary == 0 || canary == (uint64_t)-1)
        canary ^= 0xDEADBEEFCAFEBABEULL;
    canary &= 0xFFFFFFFFFFFFFF00ULL;   /* NULL-byte canary convention */
    if (canary == 0) canary = 0xDEADBEEFCAFEBE00ULL;
    __stack_chk_guard = canary;
}

__attribute__((noreturn))
void __stack_chk_fail(void) {
    // No UEFI services assumed available — just halt
    __asm__ volatile("cli");
    for (;;)
        __asm__ volatile("hlt");
}
