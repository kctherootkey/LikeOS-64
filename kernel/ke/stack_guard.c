// LikeOS-64 Stack Canary Support — kernel smash reporter

#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/sched.h>
#include <kernel/uapi/bug.h>

// Fallback guard symbol for any kernel object compiled with =global guard mode.
// Active kernel code uses GS:104 (per-CPU) via -mstack-protector-guard=tls.
uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

/* ---- helpers (all no_stack_protector to stay re-entrant) ---- */

static int __attribute__((no_stack_protector)) _ksc_is_kern(uint64_t a)
{
	return a >= 0xffff800000000000ULL && a < 0xfffffffffffff000ULL;
}

static void __attribute__((no_stack_protector)) _ksc_trace(uint64_t rbp,
							   uint64_t rip)
{
	kprintf("Call Trace:\n");
	kprintf("  [<%016llx>]\n", rip);
	int depth = 0;
	while (_ksc_is_kern(rbp) && depth < 16) {
		uint64_t *f;
		uint64_t ret, up;

		/* The frame has to be READABLE before it is read.
		 *
		 * A frame chain that runs to the top of a kernel stack leaves
		 * the last rbp one word below the guard page, so reading
		 * [rbp+8] faults -- and a fault HERE cannot be recovered from:
		 * this runs from the oops path and from __stack_chk_fail, so
		 * the second fault comes back as "recursive entry on CPU 0 --
		 * halting" and takes the machine down instead of printing a
		 * trace.  A might_sleep() report from softirq context
		 * (net_rx_softirq -> tcp_rx -> slab_free -> the page-batch
		 * free) did exactly that: a DEBUG warning became a halt.
		 *
		 * mm_user_addr_mapped() is not user-only -- it walks the
		 * current CR3 by index and answers for any address, kernel
		 * ones included.  Read-only and lock-free, which is what a
		 * crash path needs. */
		if ((rbp & 7) || !mm_user_addr_mapped(rbp, 16))
			break;
		f = (uint64_t *)rbp;
		ret = f[1]; /* [rbp+8] = return address */
		up = f[0]; /* [rbp+0] = saved RBP     */
		if (!ret || !_ksc_is_kern(ret))
			break;
		kprintf("  [<%016llx>]\n", ret);
		if (up <= rbp)
			break;
		rbp = up;
		depth++;
	}
}

static void __attribute__((no_stack_protector)) _ksc_dump(uint64_t rsp)
{
	kprintf("Stack dump:\n");
	if (!_ksc_is_kern(rsp))
		return;
	uint8_t *p = (uint8_t *)rsp;
	for (int row = 0; row < 8; row++) {
		if (!_ksc_is_kern((uint64_t)(p + row * 8)))
			break;
		kprintf("  %016llx: %02x %02x %02x %02x %02x %02x %02x %02x\n",
			(uint64_t)(p + row * 8), p[row * 8 + 0], p[row * 8 + 1],
			p[row * 8 + 2], p[row * 8 + 3], p[row * 8 + 4],
			p[row * 8 + 5], p[row * 8 + 6], p[row * 8 + 7]);
	}
}

static const char *__attribute__((no_stack_protector)) _ksc_pattern(uint64_t c)
{
	uint32_t lo = (uint32_t)c;
	if (lo == 0x41414141U)
		return "ASCII overwrite ('A')";
	if (lo == 0x42424242U)
		return "ASCII overwrite ('B')";
	if (lo == 0xCCCCCCCCU)
		return "debug poison / uninit fill";
	if (c == 0)
		return "zero write (memset or string terminator)";
	return NULL;
}

/* ---- might_sleep() reporter ----
 *
 * might_sleep() fires deep inside whatever sleeping primitive was reached, and
 * that name is never the useful one: every filesystem semaphore looks the same
 * from there, and the question is always which entry point took a spinlock (or
 * arrived from an interrupt) and then called into the filesystem.  So report
 * the caller chain as well, in the same form as the oops "Call Trace", to be
 * resolved with
 *   addr2line -f -e build/kernel.elf <addr> ...
 * (build with NO_STRIP=1 to keep the symbols).  Rate-limited: a path that does
 * this once normally does it on every iteration of a loop. */
void __attribute__((no_stack_protector))
bug_report_atomic_sleep(const char *file, int line, const char *func)
{
	static int count = 0;
	if (count >= 10)
		return;
	count++;

	uint64_t rbp;
	__asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

	task_t *cur = read_gs_base_msr() ? sched_current() : NULL;

	kprintf("WARNING: might_sleep() called with IRQs disabled at %s:%d %s()"
		" [%s pid=%d]\n",
		file, line, func, cur ? (cur->comm[0] ? cur->comm : "(anon)") : "(no task)",
		cur ? cur->id : -1);
	_ksc_trace(rbp, (uint64_t)__builtin_return_address(0));
	if (count == 10)
		kprintf("WARNING: further might_sleep() reports suppressed\n");
}

/* ---- spinlock stall reporter ----
 *
 * spin_lock() calls this when a lock has not come free after SPIN_STALL_PAUSES
 * PAUSE instructions -- seconds, where every real hold in this kernel is
 * microseconds.  By then the waiting processor has had interrupts disabled for
 * that whole time, so it is not being preempted, is not taking timer ticks and
 * is not going to recover on its own: from the outside the machine has stopped.
 *
 * What is worth printing is the lock's NAME and the chain that reached it.  The
 * name identifies the subsystem, and the two ends of a deadlock report
 * separately (each processor is stuck on its own lock), so the pair of messages
 * names the cycle.  A single message with no counterpart means the holder is
 * not spinning at all -- it is a lock leaked by a path that returned or died
 * without releasing.
 *
 * Not re-entrant, deliberately.  kprintf() takes locks of its own (the console,
 * the serial port), so a report issued from inside the console's own lock would
 * come straight back here; the flag makes the nested call return and the outer
 * one do the printing.  It also keeps the two ends of a deadlock from
 * interleaving their traces into one unreadable block.
 *
 * Rate-limited like the might_sleep() reporter above: a stalled lock keeps
 * stalling, and the first few reports are the whole story.
 */
void __attribute__((no_stack_protector)) spin_report_stall(spinlock_t *lock)
{
	static volatile unsigned char reporting;
	static int count;

	if (count >= 8)
		return;
	if (__atomic_test_and_set(&reporting, __ATOMIC_ACQUIRE))
		return;
	count++;

	uint64_t rbp;
	__asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

	int gs_ok = (int)read_gs_base_msr();
	task_t *cur = gs_ok ? sched_current() : NULL;

	/* Address and state FIRST, and as plain integers.
	 *
	 * The name is printed separately and last, because a stalled lock is
	 * quite likely to be one that no longer exists: a lock in freed memory
	 * is never released, which is exactly how a wait ends up unbounded.
	 * Its `name' is then whatever the allocator left in that slot, and
	 * printing it faults inside kprintf -- which is what happened the first
	 * time this fired, cutting the message off at "spinlock '" and taking
	 * the useful half of the report with it.  The address alone identifies
	 * the object, and `locked=1' with a name that does not resolve is
	 * itself the diagnosis. */
	kprintf("WARNING: spinlock %016llx (locked=%u) not acquired after "
		"%u million spins; cpu %d [%s pid=%d]\n",
		(uint64_t)(uintptr_t)lock, (unsigned)lock->locked,
		(unsigned)(SPIN_STALL_PAUSES / 1000000u),
		gs_ok ? (int)this_cpu_id() : -1,
		cur ? (cur->comm[0] ? cur->comm : "(anon)") : "(no task)",
		cur ? (int)cur->id : -1);

	/* Same readability test the frame walk uses: a kernel address that the
	 * current page tables actually map.  It does not prove the bytes are a
	 * string, so the print is bounded as well. */
	const char *nm = lock->name;
	if (nm && _ksc_is_kern((uint64_t)nm) &&
	    mm_user_addr_mapped((uint64_t)nm, 1)) {
		char buf[33];
		int i = 0;
		while (i < 32 && mm_user_addr_mapped((uint64_t)(nm + i), 1) &&
		       nm[i] >= 0x20 && nm[i] < 0x7f)
			buf[i] = nm[i], i++;
		buf[i] = '\0';
		kprintf("  name '%s'%s\n", buf,
			i == 32 ? " (truncated)" : "");
	} else {
		kprintf("  name pointer %016llx is not readable -- this lock is "
			"very likely in freed memory\n",
			(uint64_t)(uintptr_t)nm);
	}

	_ksc_trace(rbp, (uint64_t)__builtin_return_address(0));
	if (count == 8)
		kprintf("WARNING: further spinlock stall reports suppressed\n");

	__atomic_clear(&reporting, __ATOMIC_RELEASE);
}

/* ---- main handler ---- */

__attribute__((noreturn, no_stack_protector)) void __stack_chk_fail(void)
{
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
	uint32_t cpu_id = 0;
	task_t *cur = NULL;
	percpu_t *pcpu = NULL;
	if (gs_ok) {
		cpu_id = this_cpu_id();
		cur = sched_current();
		pcpu = this_cpu();
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
	int is_sched_race = 0;
	{
		int found_null_term = (found != 0) && ((found & 0xFF) == 0);
		int expect_null_term =
			(expected != 0) && ((expected & 0xFF) == 0);

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
		is_sched_race = (race_cpu >= 0);

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
			/* Name the owning tasks: 'found' belongs to the task whose
			 * canary sat in GS:104 at this function's PROLOGUE — i.e.
			 * the task some switch-in path failed to replace.  Together
			 * with the current task this pinpoints which switch path
			 * skipped the canary install. */
			{
				task_t *ft = sched_task_by_canary(found);
				task_t *et = sched_task_by_canary(expected);
				kprintf("    'found' canary owner:    tid=%d comm=%s\n",
					ft ? (int)ft->id : -1,
					ft ? ft->comm : "(no live task)");
				kprintf("    'expected' canary owner: tid=%d comm=%s\n",
					et ? (int)et->id : -1,
					et ? et->comm : "(no live task)");
			}
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

	if (is_sched_race) {
		/* Confirmed scheduler-race false positive: 'found' matches
		 * another CPU's live canary, so GS:104 merely moved under this
		 * frame — no memory was actually smashed.  Do NOT wedge the CPU
		 * with cli;hlt: a halted-with-IRQs-off CPU stops acknowledging
		 * TLB-shootdown IPIs, which times out every other CPU doing a
		 * slab_free/address-space teardown and stalls the whole machine
		 * (observed as "SMP: TLB shootdown sync timeout", then cascading
		 * loopback / fork-child hangs).
		 *
		 * An sti;hlt loop is NOT enough either: kernel-mode frames are
		 * never involuntarily preempted in this design (cooperative
		 * only), so hlt keeps the CPU pinned on this dead task forever.
		 * When the victim is a real task rather than an idle task, every
		 * READY task on this CPU's runqueue is stranded behind it —
		 * observed as PID 2 parking CPU 0 (NET_RX_CPU) and starving
		 * ksoftirqd/0: rx_q grew unbounded and all networking hung
		 * system-wide.
		 *
		 * So in process context, surrender the CPU through the scheduler
		 * forever: the task stays parked in this loop (acceptable), but
		 * the runqueue keeps draining.  From IRQ context or with
		 * preemption disabled, scheduling is impossible — fall back to
		 * the interruptible halt (IPIs still serviced). */
		if (pcpu && pcpu->interrupt_nesting == 0 &&
		    pcpu->preempt_count == 0) {
			kprintf("\nFALSE POSITIVE — task parked, CPU yields (runqueue keeps draining)\n");
			kprintf("========================================\n");
			for (;;) {
				__asm__ volatile("sti" ::: "memory");
				sched_yield_in_kernel();
			}
		}
		kprintf("\nFALSE POSITIVE — CPU parked INTERRUPTIBLY (system continues, TLB/IPI serviced)\n");
		kprintf("========================================\n");
		for (;;)
			__asm__ volatile("sti; hlt");
	}

	kprintf("\nSYSTEM HALTED\n");
	kprintf("========================================\n");

	for (;;)
		__asm__ volatile("hlt");
}
