// LikeOS-64 Stack Canary Support (Security Feature)
//
// This module provides stack buffer overflow detection via stack canaries.
// When compiled with -fstack-protector-strong, the compiler inserts checks
// at function prologues/epilogues to detect stack corruption.

#include "../../include/kernel/console.h"

// ============================================================================
// Stack Canary Support
// ============================================================================
// Stack canary value used by -fstack-protector-strong
// This value is checked at function return to detect stack buffer overflows
// Using a randomized-looking value; in a production system this should be
// initialized from a hardware RNG or RDRAND instruction at boot time
uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

// Called when stack canary mismatch is detected - indicates stack corruption
// This is a critical security event indicating a buffer overflow attack
__attribute__((noreturn))
void __stack_chk_fail(void) {
    // Try to print a warning if console is available
    // Note: We use console_putchar directly as stack may be corrupted
    const char* msg = "\n*** SECURITY: Stack smashing detected! ***\n";
    while (*msg) {
        console_putchar(*msg++);
    }
    // Halt the CPU - do not continue with corrupted stack
    __asm__ volatile("cli; hlt");
    // In case hlt returns (shouldn't happen), infinite loop
    for (;;) {
        __asm__ volatile("hlt");
    }
}
