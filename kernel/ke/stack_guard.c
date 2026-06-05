// LikeOS-64 Stack Canary Support — kernel smash reporter

#include <kernel/console.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/bug.h>

// Fallback guard symbol for any kernel object compiled with =global guard mode.
// Active kernel code uses GS:104 (per-CPU) via -mstack-protector-guard=tls.
uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

/* ---- helpers (all no_stack_protector to stay re-entrant) ---- */

static int __attribute__((no_stack_protector))
_ksc_is_kern(uint64_t a) {
    return a >= 0xffff800000000000ULL && a < 0xfffffffffffff000ULL;
}

static void __attribute__((no_stack_protector))
_ksc_trace(uint64_t rbp, uint64_t rip) {
    kprintf("Call Trace:\n");
    kprintf("  [<%016llx>]\n", rip);
    int depth = 0;
    while (_ksc_is_kern(rbp) && depth < 16) {
        uint64_t *f   = (uint64_t *)rbp;
        uint64_t  ret = f[1];   /* [rbp+8] = return address */
        uint64_t  up  = f[0];   /* [rbp+0] = saved RBP     */
        if (!ret || !_ksc_is_kern(ret)) break;
        kprintf("  [<%016llx>]\n", ret);
        if (up <= rbp) break;
        rbp = up;
        depth++;
    }
}

static void __attribute__((no_stack_protector))
_ksc_dump(uint64_t rsp) {
    kprintf("Stack dump:\n");
    if (!_ksc_is_kern(rsp)) return;
    uint8_t *p = (uint8_t *)rsp;
    for (int row = 0; row < 8; row++) {
        if (!_ksc_is_kern((uint64_t)(p + row * 8))) break;
        kprintf("  %016llx: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                (uint64_t)(p + row * 8),
                p[row*8+0], p[row*8+1], p[row*8+2], p[row*8+3],
                p[row*8+4], p[row*8+5], p[row*8+6], p[row*8+7]);
    }
}

static const char * __attribute__((no_stack_protector))
_ksc_pattern(uint64_t c) {
    uint32_t lo = (uint32_t)c;
    if (lo == 0x41414141U) return "ASCII overwrite ('A')";
    if (lo == 0x42424242U) return "ASCII overwrite ('B')";
    if (lo == 0xCCCCCCCCU) return "debug poison / uninit fill";
    if (c  == 0)           return "zero write (memset or string terminator)";
    return NULL;
}

/* ---- main handler ---- */

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void) {
    __asm__ volatile("cli" ::: "memory");

    /* Capture registers before touching anything else. */
    uint64_t rip = (uint64_t)__builtin_return_address(0);
    uint64_t rsp, rbp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    /* Expected canary: current per-CPU value at GS:104. */
    uint64_t expected = 0;
    int gs_ok = (int)read_gs_base_msr();
    if (gs_ok)
        __asm__ volatile("mov %%gs:104, %0" : "=r"(expected));

    /* Found (possibly corrupted) canary: GCC stores it at [caller_rbp - 8]. */
    uint64_t found = 0;
    void *caller_frame = __builtin_frame_address(1);
    if (caller_frame && _ksc_is_kern((uint64_t)caller_frame))
        found = *(uint64_t *)((uint8_t *)caller_frame - 8);

    /* CPU / task identification. */
    uint32_t   cpu_id = 0;
    task_t    *cur    = NULL;
    percpu_t  *pcpu   = NULL;
    if (gs_ok) {
        cpu_id = this_cpu_id();
        cur    = sched_current();
        pcpu   = this_cpu();
    }

    /* Interrupt state from RFLAGS. */
    uint64_t rflags = 0;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));

    console_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n========================================\n");
    kprintf("KERNEL STACK SMASH DETECTED\n\n");

    kprintf("CPU:    %u\n", cpu_id);
    if (cur) {
        kprintf("TASK:   %s\n", cur->comm[0] ? cur->comm : "(anon)");
        kprintf("PID:    %d\n", cur->tgid);
        kprintf("THREAD: %016llx\n", (uint64_t)(uintptr_t)cur);
    } else {
        kprintf("TASK:   (none — early boot)\n");
    }

    kprintf("\nRIP:    %016llx\n", rip);
    kprintf("RSP:    %016llx\n", rsp);
    kprintf("RBP:    %016llx\n", rbp);

    kprintf("\nEXPECTED CANARY:\n  %016llx\n", expected);
    kprintf("\nFOUND CANARY:\n  %016llx\n", found);

    const char *pat = _ksc_pattern(found);
    if (pat)
        kprintf("\nOverflow pattern: %s\n", pat);

    /* ------------------------------------------------------------------ *
     * DIAGNOSIS: distinguish a real stack overflow from a scheduler race. *
     *                                                                      *
     * generate_stack_canary() always zeroes the low byte.  A genuine      *
     * overflow almost never produces a value with a null low byte (unless  *
     * it happens to be a null-terminated string write).  If 'found' has    *
     * a non-zero low byte it cannot be a valid kernel canary → real bug.   *
     *                                                                      *
     * If 'found' does look like a valid canary we scan all online CPUs:    *
     * if any CPU's current GS:104 equals 'found' it means the function     *
     * saved that canary at its prologue, but the scheduler changed GS:104   *
     * to a different task's canary before the epilogue ran → false positive. *
     * ------------------------------------------------------------------ */
    {
        int found_null_term  = (found   != 0) && ((found   & 0xFF) == 0);
        int expect_null_term = (expected != 0) && ((expected & 0xFF) == 0);

        int race_cpu = -1;
        if (gs_ok && found_null_term) {
            uint32_t ncpus = percpu_get_online_count();
            for (uint32_t i = 0; i < ncpus && i < MAX_CPUS; i++) {
                percpu_t *c = percpu_get(i);
                if (c && c->stack_canary == found) {
                    race_cpu = (int)i;
                    break;
                }
            }
        }

        kprintf("\n--- DIAGNOSIS ---\n");
        if (!found_null_term) {
            kprintf("*** REAL BUG: 'found' canary has a non-zero low byte and cannot\n");
            kprintf("    be a valid kernel canary (generate_stack_canary zeros byte 0).\n");
            kprintf("    This is almost certainly a genuine stack buffer overflow.\n");
        } else if (race_cpu >= 0) {
            kprintf("*** SCHEDULER RACE (likely false positive):\n");
            kprintf("    'found' (%016llx) matches CPU %d's current active canary.\n",
                    found, race_cpu);
            kprintf("    GS:104 was updated by a context switch between this function's\n");
            kprintf("    prologue (saved 'found') and epilogue (read 'expected').\n");
            kprintf("    No actual stack smash occurred.\n");
            kprintf("    Fix: ensure the scheduler writes task canary to GS:104 BEFORE\n");
            kprintf("    re-enabling interrupts (sti) in sched_schedule/sched_run_ready.\n");
        } else if (found_null_term && expect_null_term && pat == NULL) {
            kprintf("??? AMBIGUOUS: both canaries have valid format (null low byte,\n");
            kprintf("    no recognizable pattern).  Possible scheduler race where the\n");
            kprintf("    CPU has already moved on (canary no longer active on any CPU),\n");
            kprintf("    or a genuine overflow with a well-formatted value.\n");
            kprintf("    Check the call trace: a syscall path interrupted by a timer\n");
            kprintf("    preempt points to a scheduler race; a buffer-heavy function\n");
            kprintf("    at the top of the trace points to a real overflow.\n");
        } else {
            kprintf("*** LIKELY REAL BUG: pattern or format analysis suggests genuine\n");
            kprintf("    stack corruption.  Inspect the call trace for the offending\n");
            kprintf("    buffer write.\n");
        }
    }

    kprintf("\nInterrupt state:\n");
    kprintf("  interrupts=%-8s  in_irq=%-3s  preempt_count=%d\n",
            (rflags >> 9) & 1 ? "enabled" : "disabled",
            (pcpu && pcpu->interrupt_nesting > 0) ? "yes" : "no",
            pcpu ? pcpu->preempt_count : 0);

    kprintf("\n");
    _ksc_trace(rbp, rip);
    kprintf("\n");
    _ksc_dump(rsp);

    kprintf("\nSYSTEM HALTED\n");
    kprintf("========================================\n");

    for (;;)
        __asm__ volatile("hlt");
}
