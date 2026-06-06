// Stack canary support for ld-likeos.so
// Must be self-contained — libc is not available during rtld startup.

unsigned long long __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

__attribute__((noreturn, no_stack_protector, visibility("hidden")))
void __stack_chk_fail(void) {
    static const char msg[] = "*** stack smashing detected ***\nprocess: ld-likeos.so (rtld)\nProcess aborted.\n";
    __asm__ volatile("syscall" :
        : "a"(1L), "D"(2L), "S"(msg), "d"((long)(sizeof(msg) - 1))
        : "rcx", "r11", "memory");
    /* SYS_exit_group(127) — LikeOS syscall 312 */
    __asm__ volatile("syscall" :
        : "a"(312L), "D"(127L)
        : "rcx", "r11");
    /* Fallback: halt the CPU if exit_group somehow returns */
    for (;;)
        __asm__ volatile("hlt" ::: "memory");
}
