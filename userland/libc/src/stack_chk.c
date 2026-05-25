// Stack canary support for libc (-fstack-protector-strong)
// Uses fs:0x28 (TLS slot) as the canary source — set per-process at exec time.
// __stack_chk_guard is kept for linking against objects compiled with =global.
#include <stdint.h>

uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

/* ---- raw I/O helpers (no libc — stack may be corrupt) ---- */

static __attribute__((noinline, no_stack_protector))
void _sc_wbuf(const char *b, long n) {
    __asm__ volatile("syscall"
        : : "a"(1L), "D"(2L), "S"(b), "d"(n)
        : "rcx", "r11", "memory");
}

static __attribute__((no_stack_protector))
void _sc_ws(const char *s) {
    long n = 0;
    while (s[n]) n++;
    _sc_wbuf(s, n);
}

static __attribute__((no_stack_protector))
void _sc_wdec(uint64_t v) {
    char buf[22];
    int i = 21;
    buf[21] = '\n';
    if (!v) { buf[20] = '0'; i = 20; }
    else { while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; } }
    _sc_wbuf(buf + i, 22 - i);
}

/* Program name — set from argv[0] via __libc_set_progname() in crt0.S,
 * defined in src/stdio/err.c. */
extern const char *__progname;

#ifdef STACK_SMASH_DEBUG

static int __attribute__((no_stack_protector))
_sc_is_user(uint64_t a) {
    return a >= 0x1000ULL && a < 0x00007ffffffff000ULL;
}

static __attribute__((no_stack_protector))
void _sc_whex(uint64_t v) {
    /* "    0x" + 16 hex chars + "\n" = 23 bytes */
    static const char h[] = "0123456789abcdef";
    char buf[23];
    buf[0] = ' '; buf[1] = ' '; buf[2] = ' '; buf[3] = ' ';
    buf[4] = '0'; buf[5] = 'x';
    for (int i = 21; i >= 6; i--, v >>= 4)
        buf[i] = h[v & 0xf];
    buf[22] = '\n';
    _sc_wbuf(buf, 23);
}

static void __attribute__((no_stack_protector))
_sc_trace(uint64_t rbp) {
    _sc_ws("Call Trace:\n");
    int depth = 0;
    while (_sc_is_user(rbp) && depth < 20) {
        uint64_t *f   = (uint64_t *)rbp;
        uint64_t  ret = f[1];
        uint64_t  up  = f[0];
        if (!_sc_is_user(ret)) break;
        _sc_ws("    ");
        _sc_whex(ret);
        if (!_sc_is_user(up) || up <= rbp) break;
        rbp = up;
        depth++;
    }
}

static void __attribute__((no_stack_protector))
_sc_dump(uint64_t rsp) {
    static const char h[] = "0123456789abcdef";
    _sc_ws("Stack Memory Dump:\n");
    if (!_sc_is_user(rsp)) return;
    uint8_t *p = (uint8_t *)rsp;
    for (int row = 0; row < 4; row++) {
        if (!_sc_is_user((uint64_t)(p + row * 8))) break;
        char line[58];
        uint64_t addr = (uint64_t)(p + row * 8);
        line[0] = ' '; line[1] = ' '; line[2] = ' '; line[3] = ' ';
        line[4] = '0'; line[5] = 'x';
        for (int i = 21; i >= 6; i--, addr >>= 4) line[i] = h[addr & 0xf];
        line[22] = ':'; line[23] = ' ';
        int pos = 24;
        for (int b = 0; b < 8; b++) {
            uint8_t v = p[row * 8 + b];
            line[pos++] = h[v >> 4];
            line[pos++] = h[v & 0xf];
            line[pos++] = (b < 7) ? ' ' : '\n';
        }
        _sc_wbuf(line, pos);
    }
}

static const char * __attribute__((no_stack_protector))
_sc_pattern(uint64_t c) {
    uint32_t lo = (uint32_t)c;
    if (lo == 0x41414141U) return "ASCII overwrite ('A')";
    if (lo == 0x42424242U) return "ASCII overwrite ('B')";
    if (lo == 0xCCCCCCCCU) return "debug poison / uninit fill";
    if (c  == 0)           return "zero write (memset or string terminator)";
    return (const char *)0;
}

#endif /* STACK_SMASH_DEBUG */

/* ---- public handler ---- */

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void) {
    /* MUST be first: at entry, RAX = saved_canary - %fs:0x28 (epilogue
     * check: mov [slot],%rax; sub %fs:0x28,%rax; jne __stack_chk_fail).
     * Capture RAX before any syscall clobbers it with a return value.
     * x86-64 function prologues never modify RAX, so this is safe. */
    uint64_t canary_diff;
    __asm__ volatile("mov %%rax, %0" : "=r"(canary_diff));

    /* Expected canary: current %fs:0x28 at time of epilogue check. */
    uint64_t expected;
    __asm__ volatile("mov %%fs:0x28, %0" : "=r"(expected));

    /* Actual saved canary (may be corrupted): diff + expected. */
    uint64_t found = canary_diff + expected;

    /* PID / TID via direct syscall. */
    long pid, tid;
    __asm__ volatile("syscall" : "=a"(pid) : "0"(39L)  : "rcx", "r11");
    __asm__ volatile("syscall" : "=a"(tid) : "0"(313L) : "rcx", "r11"); /* SYS_GETTID */

#ifndef STACK_SMASH_DEBUG
    /* ---- minimal output ---- */
    _sc_ws("*** stack smashing detected ***\n");
    _sc_ws("process: "); _sc_ws(__progname); _sc_ws("\n");
    _sc_ws("pid: ");     _sc_wdec((uint64_t)pid);
    _sc_ws("thread: ");  _sc_wdec((uint64_t)tid);
    _sc_ws("Process aborted.\n");
#else
    /* ---- verbose DEBUG=1 output ---- */
    uint64_t rip = (uint64_t)__builtin_return_address(0);
    uint64_t rsp, rbp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    _sc_ws("\n*** USERLAND STACK SMASH DETECTED ***\n\n");

    _sc_ws("Process: "); _sc_ws(__progname); _sc_ws("\n");
    _sc_ws("PID:     "); _sc_wdec((uint64_t)pid);
    _sc_ws("TID:     "); _sc_wdec((uint64_t)tid);

    _sc_ws("\nSignal:\n    SIGABRT (stack protector)\n");

    _sc_ws("\nFaulting thread:\n    ");
    _sc_ws(pid == tid ? "main thread" : "worker thread");
    _sc_ws("\n");

    _sc_ws("\nInstruction Pointer:\n    RIP: ");
    _sc_whex(rip);

    _sc_ws("\nStack Pointer:\n    RSP: ");
    _sc_whex(rsp);
    _sc_ws("    RBP: ");
    _sc_whex(rbp);

    _sc_ws("\nExpected Canary (current %fs:0x28):\n");
    _sc_whex(expected);

    _sc_ws("\nSaved Canary (stack slot, may be corrupted):\n");
    _sc_whex(found);

    /* Diagnosis */
    if (found == 0 && expected != 0) {
        _sc_ws("\nDiagnosis: stack slot zeroed — likely buffer overflow with NUL bytes\n");
    } else if (found != 0 && expected == 0) {
        _sc_ws("\nDiagnosis: %fs:0x28 is ZERO — FS base was reset/corrupted (TLS bug)\n");
    } else if (found == expected) {
        _sc_ws("\nDiagnosis: canaries match — impossible? (diff was non-zero at call site)\n");
    } else {
        _sc_ws("\nDiagnosis: canary mismatch — stack corrupted or FS base changed\n");
        const char *pat = _sc_pattern(found);
        if (pat) {
            _sc_ws("Overflow pattern: ");
            _sc_ws(pat); _sc_ws("\n");
        }
    }

    _sc_ws("\n");
    _sc_trace(rbp);

    _sc_ws("\n");
    _sc_dump(rsp);

    _sc_ws("\nTerminating process.\n\n");
    _sc_ws("==================================================\n");
#endif

    /* SYS_exit_group(127) — LikeOS syscall 312 */
    __asm__ volatile("syscall"
        : : "a"(312L), "D"(127L) : "rcx", "r11");
    /* Fallback: halt the CPU if exit_group somehow returns */
    for (;;)
        __asm__ volatile("hlt" ::: "memory");
}
