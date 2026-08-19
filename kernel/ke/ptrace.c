// LikeOS-64 -- ptrace.
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/rwsem.h>
#include <kernel/fs/icache.h>
#include <kernel/net/net.h>
#include <kernel/ke/uaccess.h>

/* The rule that used to live here — may the caller read another process's
 * private state — is now task_may_access(target, ACCESS_READ) in cred.c, so
 * that ptrace and the /proc-equivalent syscalls below cannot drift apart, and
 * so that it is applied to a target's whole thread group rather than to
 * whichever thread the caller happened to name.  See cred.h. */

// SYS_GETPROCINFO - retrieve info about all processes
// a1 = pointer to user-space procinfo_t array
// a2 = max number of entries the array can hold
// Returns: number of entries filled, or negative error
/* SYS_GETPROCMAPS - report one process's address space.
 *
 * ps can only show a total, and a total cannot tell a table filling up with
 * records from a few records that are growing, which are different faults with
 * different fixes.  This hands out the region table itself plus the brk span,
 * so the question "what exactly is growing" is answerable from userspace
 * instead of by rebuilding the kernel with a printf in it.
 *
 * Reported for the PROCESS: the bookkeeping lives on the thread group leader
 * (task_mm_owner), so asking about any thread answers for the address space it
 * shares.
 *
 * Restricted to processes the caller owns (task_may_access, ACCESS_READ); root
 * sees every process.  The report is a map of where another process keeps its
 * code, its stacks and its heap, so it is not something one user may take from
 * another.
 */
/* ============================ ptrace(2) ==================================== */

/* Request numbers.  Mirrored in user/lib/libc/include/sys/ptrace.h, which is
 * also where the interface is documented; keep the two in step. */
#define PT_TRACEME 0
#define PT_PEEKTEXT 1
#define PT_PEEKDATA 2
#define PT_POKETEXT 3
#define PT_POKEDATA 4
#define PT_GETREGS 5
#define PT_SETREGS 6
#define PT_GETFPREGS 7
#define PT_SETFPREGS 8
#define PT_CONT 9
#define PT_SINGLESTEP 10
#define PT_SYSCALL 11
#define PT_ATTACH 12
#define PT_DETACH 13
#define PT_KILL 14
#define PT_PEEKUSER 15
#define PT_POKEUSER 16
#define PT_SETOPTIONS 17
#define PT_GETEVENTMSG 18
#define PT_GETAUXV 19
#define PT_GETDBREGS 20
#define PT_SETDBREGS 21
#define PT_GETSIGINFO 22
#define PT_GETNUMLWPS 23
#define PT_GETLWPLIST 24
#define PT_GETEVENT 25
#define PT_GETEXECPATH 26

/* Debug-register control word, bit by bit, because every one of them either
 * has to be forced or has to be refused.  See ptrace_sanitize_dr7. */
#define DR7_LOCAL_SLOT(n) (1ULL << ((n) * 2))
#define DR7_GLOBAL_SLOT(n) (1ULL << ((n) * 2 + 1))
#define DR7_ENABLE_MASK 0x000000FFULL /* L0,G0 .. L3,G3 */
#define DR7_LE_GE_MASK 0x00000300ULL  /* bits 8,9: legacy, harmless */
#define DR7_MBO 0x00000400ULL	      /* bit 10 reads as 1 */
#define DR7_GD 0x00002000ULL	      /* bit 13: General Detect */
#define DR7_RWLEN_SHIFT 16
#define DR7_RWLEN_MASK 0xFFFF0000ULL /* RW/LEN, four bits per slot */

/* RW field values.  10b is an I/O breakpoint, which needs CR4.DE and watches
 * port space rather than memory; it is not offered. */
#define DR_RW_EXEC 0x0
#define DR_RW_WRITE 0x1
#define DR_RW_IO 0x2
#define DR_RW_RDWR 0x3

#define PTRACE_O_ALL 0x003F

/*
 * Reduce a tracer-supplied DR7 to the bits this kernel is willing to honour.
 *
 * A debug register is not like the other registers a tracer can set.  The
 * others describe the tracee; these describe what the PROCESSOR does, in
 * whatever context it happens to be in, and two of the ways they can be set
 * reach past the tracee entirely:
 *
 *   - GD (bit 13) makes any access to a debug register raise #DB.  The kernel
 *     accesses them on every context switch involving a watchpoint, so a tracee
 *     with GD set would fault the kernel, inside the handler, forever.  It is
 *     cleared unconditionally.
 *
 *   - An address is compared against every access the CPU makes, including the
 *     kernel's own while it is running on this task's behalf.  Kernel addresses
 *     are refused in ptrace_set_dbregs for that reason; what stays here is the
 *     control word.
 *
 * I/O breakpoints (RW=10b) are refused because they need CR4.DE and watch port
 * space; an execute breakpoint is forced to LEN=0 because any other length is
 * architecturally undefined.  A slot asking for something not allowed is
 * disabled rather than corrected: a watchpoint that silently watches the wrong
 * thing is worse than one the debugger can see did not take.
 */
static uint64_t ptrace_sanitize_dr7(uint64_t dr7)
{
	uint64_t out = (dr7 & (DR7_ENABLE_MASK | DR7_LE_GE_MASK)) | DR7_MBO;

	for (int slot = 0; slot < 4; slot++) {
		unsigned field = (unsigned)((dr7 >> (DR7_RWLEN_SHIFT +
						     slot * 4)) & 0xF);
		unsigned rw = field & 0x3;
		unsigned len = (field >> 2) & 0x3;

		if (!(out & (DR7_LOCAL_SLOT(slot) | DR7_GLOBAL_SLOT(slot))))
			continue; /* Not enabled; its RW/LEN mean nothing. */

		if (rw == DR_RW_IO) {
			out &= ~(DR7_LOCAL_SLOT(slot) | DR7_GLOBAL_SLOT(slot));
			continue;
		}
		if (rw == DR_RW_EXEC)
			len = 0;

		out |= ((uint64_t)(rw | (len << 2)))
		       << (DR7_RWLEN_SHIFT + slot * 4);
	}

	/* GD, and every reserved bit, are absent by construction: `out' was
	 * built from the fields above rather than masked out of the input. */
	return out;
}

/* Resolve a tracee for a request from `cur', enforcing the whole access
 * policy, and return it with g_task_list_lock HELD so the caller can act on it
 * without the answer going stale.  *flags receives the saved interrupt state.
 *
 * On any refusal the lock is dropped and the errno returned through *err.
 *
 * Re-run for every request rather than trusting the attach.  A tracee can
 * change identity while it is being traced -- it can exec, and it can call
 * setuid if it holds a saved uid that allows it -- and a decision taken at
 * attach time would still be saying yes long after the answer changed.  The
 * check is a handful of integer comparisons; caching it buys nothing worth
 * that hole. */
static task_t *ptrace_lock_tracee(task_t *cur, uint64_t pid, uint64_t *flags,
				  int64_t *err)
{
	/* The tracer is the PROCESS, not the thread that happened to make the
	 * call.  A debugger is entitled to attach from one thread and drive the
	 * tracee from another, and PTRACE_TRACEME records the parent -- which
	 * is always a group leader -- so recording the calling thread here
	 * instead would make those two disagree for a threaded tracer. */
	task_t *tracer = cur->group_leader ? cur->group_leader : cur;

	spin_lock_irqsave(&g_task_list_lock, flags);

	task_t *t = sched_find_task_by_id_locked((uint32_t)pid);
	if (!t) {
		spin_unlock_irqrestore(&g_task_list_lock, *flags);
		*err = -ESRCH;
		return NULL;
	}

	/* Only the process that actually attached may drive the tracee.
	 * Without this any process passing the credential check could steer
	 * somebody else's debugging session. */
	if (t->tracer_pid != (int)tracer->id ||
	    t->tracer_incarnation != tracer->incarnation) {
		spin_unlock_irqrestore(&g_task_list_lock, *flags);
		*err = -ESRCH;
		return NULL;
	}

	int perm = task_may_access(t, ACCESS_ATTACH);
	if (perm != 0) {
		spin_unlock_irqrestore(&g_task_list_lock, *flags);
		*err = perm;
		return NULL;
	}

	return t;
}

/* The register set handed to and taken from userspace.  Mirrors
 * `struct ptrace_regs' in user/lib/libc/include/sys/ptrace.h -- keep in step. */
typedef struct {
	uint64_t r15, r14, r13, r12;
	uint64_t rbp, rbx;
	uint64_t r11, r10, r9, r8;
	uint64_t rax, rcx, rdx, rsi, rdi;
	uint64_t rip, cs, rflags, rsp, ss;
	uint64_t fs_base, gs_base;
	uint64_t ds, es, fs, gs;
} ptrace_regs_t;

/* The two declarations live in different builds and cannot be checked against
 * each other directly, so both are pinned to the same size.  A field added to
 * one and forgotten in the other then fails to compile instead of silently
 * shifting every register a debugger reads. */
_Static_assert(sizeof(ptrace_regs_t) == 26 * 8,
	       "ptrace_regs_t must match struct ptrace_regs in sys/ptrace.h");

/* Fill `out' with a stopped tracee's user registers.
 *
 * Where they live depends on how the tracee entered the kernel, and the two
 * cases differ in how complete they are:
 *
 *   - Interrupted or trapped (a breakpoint, a fault, a timer preemption):
 *     the whole set was pushed by the interrupt stub and preempt_frame points
 *     at it.  Everything is exact.
 *
 *   - Inside a syscall: RCX and R11 no longer hold the user's values at all --
 *     the SYSCALL instruction overwrites them with RIP and RFLAGS -- so they
 *     are reported as what the hardware put there, which is what the user's
 *     RIP and RFLAGS actually are.  The rest comes from the per-task snapshot
 *     syscall_handler takes.
 *
 * Runs with g_task_list_lock held; only reads task fields. */
static void ptrace_read_regs(const task_t *t, ptrace_regs_t *out)
{
	mm_memset(out, 0, sizeof(*out));

	const interrupt_frame_t *f = t->preempt_frame;

	if (f) {
		out->r15 = f->r15;
		out->r14 = f->r14;
		out->r13 = f->r13;
		out->r12 = f->r12;
		out->r11 = f->r11;
		out->r10 = f->r10;
		out->r9 = f->r9;
		out->r8 = f->r8;
		out->rbp = f->rbp;
		out->rdi = f->rdi;
		out->rsi = f->rsi;
		out->rdx = f->rdx;
		out->rcx = f->rcx;
		out->rbx = f->rbx;
		out->rax = f->rax;
		out->rip = f->rip;
		out->cs = f->cs;
		out->rflags = f->rflags;
		out->rsp = f->rsp;
		out->ss = f->ss;
	} else if (t->syscall_frame) {
		/* Read from the live frame rather than the snapshot: it is what
		 * the return path will actually restore from, so it is also
		 * what a tracer must see if a write to it is to make sense. */
		const syscall_user_frame_t *s = t->syscall_frame;

		out->r15 = s->r15;
		out->r14 = s->r14;
		out->r13 = s->r13;
		out->r12 = s->r12;
		out->r10 = s->r10;
		out->r9 = s->r9;
		out->r8 = s->r8;
		out->rbp = s->rbp;
		out->rdi = s->rdi;
		out->rsi = s->rsi;
		out->rdx = s->rdx;
		out->rbx = s->rbx;
		out->rax = s->rax;
		out->rip = s->rip;
		out->rsp = s->rsp;
		out->rflags = s->rflags;
		/* Destroyed by the SYSCALL instruction itself; these are what
		 * the hardware left in them. */
		out->rcx = s->rip;
		out->r11 = s->rflags;
		out->cs = 0x23; /* user segments are fixed here */
		out->ss = 0x1B;
	} else {
		/* Neither frame available -- the task is stopped somewhere with
		 * no user context to report, so report the snapshot and say so
		 * by leaving the rest zero. */
		out->rip = t->syscall_rip;
		out->rsp = t->syscall_rsp;
		out->rflags = t->syscall_rflags;
		out->rbp = t->syscall_rbp;
		out->rbx = t->syscall_rbx;
		out->rax = t->syscall_rax;
		out->r12 = t->syscall_r12;
		out->r13 = t->syscall_r13;
		out->r14 = t->syscall_r14;
		out->r15 = t->syscall_r15;
		if (t->syscall_regs_valid) {
			out->rdi = t->syscall_rdi;
			out->rsi = t->syscall_rsi;
			out->rdx = t->syscall_rdx;
			out->r8 = t->syscall_r8;
			out->r9 = t->syscall_r9;
			out->r10 = t->syscall_r10;
		}
		out->rcx = t->syscall_rip;
		out->r11 = t->syscall_rflags;
		out->cs = 0x23;
		out->ss = 0x1B;
	}

	/* The segment BASES, which unlike the selectors are real per-task
	 * state: FS's is where a thread's TLS block lives, and without it a
	 * debugger cannot find a thread-local variable at all.  GS's is only
	 * ever what the program asked for through arch_prctl -- the kernel
	 * keeps %gs for its own per-CPU data and never installs it -- but that
	 * is what a debugger wants to be told, and it used to be reported as
	 * zero for every task because this line was missing. */
	out->fs_base = t->fs_base;
	out->gs_base = t->gs_base;

	/* The data-segment SELECTORS, snapshotted on the way into the kernel.
	 * Zero for a task that has never been to user mode, which is the true
	 * answer for one that has no user context to report. */
	out->ds = t->useg_ds;
	out->es = t->useg_es;
	out->fs = t->useg_fs;
	out->gs = t->useg_gs;
}

/* Write a tracee's user registers back.
 *
 * RFLAGS is filtered rather than taken as given: it is the one field here that
 * can hand a process privilege it did not have.  Letting a tracer clear IF
 * would return the tracee to userspace with interrupts disabled, and setting
 * IOPL would grant port access -- neither is a debugger's to give, and both
 * would be granted by a straight copy. */
static int64_t ptrace_write_regs(task_t *t, const ptrace_regs_t *in)
{
	interrupt_frame_t *f = t->preempt_frame;

	if (f) {
		f->r15 = in->r15;
		f->r14 = in->r14;
		f->r13 = in->r13;
		f->r12 = in->r12;
		f->r11 = in->r11;
		f->r10 = in->r10;
		f->r9 = in->r9;
		f->r8 = in->r8;
		f->rbp = in->rbp;
		f->rdi = in->rdi;
		f->rsi = in->rsi;
		f->rdx = in->rdx;
		f->rcx = in->rcx;
		f->rbx = in->rbx;
		f->rax = in->rax;
		f->rip = in->rip;
		f->rsp = in->rsp;
		f->rflags = user_rflags_sanitize(in->rflags);
		/* CS/SS are deliberately not writable: a tracee that returns
		 * through a segment selector of the tracer's choosing is a way
		 * out of user mode, not a debugging feature. */
	} else if (t->syscall_frame) {
		/* Written into the live frame, because that is what the return
		 * path restores from.  Writing the snapshot instead reads back
		 * correctly and is then discarded on return -- the change
		 * appears to have been made and simply does not happen. */
		syscall_user_frame_t *s = t->syscall_frame;

		s->r15 = in->r15;
		s->r14 = in->r14;
		s->r13 = in->r13;
		s->r12 = in->r12;
		s->r10 = in->r10;
		s->r9 = in->r9;
		s->r8 = in->r8;
		s->rbp = in->rbp;
		s->rdi = in->rdi;
		s->rsi = in->rsi;
		s->rdx = in->rdx;
		s->rbx = in->rbx;
		s->rax = in->rax;
		s->rip = in->rip;
		s->rsp = in->rsp;
		s->rflags = user_rflags_sanitize(in->rflags);

		/* Keep the snapshot consistent with it: signal delivery builds
		 * its frame from these, so leaving them stale would resume a
		 * handler at the old address. */
		t->syscall_rip = s->rip;
		t->syscall_rsp = s->rsp;
		t->syscall_rflags = s->rflags;
		t->syscall_rbp = s->rbp;
		t->syscall_rbx = s->rbx;
		t->syscall_rax = s->rax;
		t->syscall_r12 = s->r12;
		t->syscall_r13 = s->r13;
		t->syscall_r14 = s->r14;
		t->syscall_r15 = s->r15;
	} else {
		/* No frame: an exec stop.  The snapshot is not a stale copy
		 * there, it is where the jump into the new image reads its
		 * entry point, stack and flags from -- so writing it is how a
		 * tracer redirects a program before its first instruction. */
		t->syscall_rip = in->rip;
		t->syscall_rsp = in->rsp;
		t->syscall_rflags = user_rflags_sanitize(in->rflags);
		t->syscall_rbp = in->rbp;
		t->syscall_rbx = in->rbx;
		t->syscall_rax = in->rax;
		t->syscall_r12 = in->r12;
		t->syscall_r13 = in->r13;
		t->syscall_r14 = in->r14;
		t->syscall_r15 = in->r15;
		t->syscall_rdi = in->rdi;
		t->syscall_rsi = in->rsi;
		t->syscall_rdx = in->rdx;
		t->syscall_r8 = in->r8;
		t->syscall_r9 = in->r9;
		t->syscall_r10 = in->r10;
	}

	/* The segment bases are task state rather than frame state, so they are
	 * written the same way whichever frame the tracee stopped with.  FS's
	 * is applied by the context switch when the tracee runs again.
	 *
	 * Refused above the user half for the reason ARCH_SET_FS refuses it:
	 * an FS base in the kernel half turns every `mov %fs:0x28, %rax' in
	 * libc into a fault at a kernel address.  That costs a tracer nothing
	 * it did not already have -- it can already write any register and any
	 * mapped byte -- and it keeps one way of setting the base from being
	 * weaker than the other.
	 *
	 * The four data-segment SELECTORS are deliberately not written, for the
	 * same reason CS and SS are not: nothing restores them on the way back
	 * to user mode, so a value accepted here would be a value that never
	 * took effect.  See the useg_* fields in <ke/sched.h>. */
	if (in->fs_base == 0 || (in->fs_base >> 47) == 0)
		task_set_fs_base(t, in->fs_base);
	if (in->gs_base == 0 || (in->gs_base >> 47) == 0)
		t->gs_base = in->gs_base;
	return 0;
}

/* Set or clear the trap flag in whichever frame the tracee will resume from.
 *
 * With TF set the processor raises #DB after the next instruction, which is
 * how single-stepping is done -- there is no other mechanism.  It has to be
 * written where the resume actually reads RFLAGS from, which is the exception
 * frame for a trapped tracee and the syscall frame for one stopped in a
 * syscall; the per-task snapshot is not consulted by either return path.
 *
 * Runs with g_task_list_lock held. */
static int64_t ptrace_set_tf(task_t *t, bool on)
{
	uint64_t *rflags = NULL;

	if (t->preempt_frame)
		rflags = &t->preempt_frame->rflags;
	else if (t->syscall_frame)
		rflags = &t->syscall_frame->rflags;

	if (rflags) {
		if (on)
			*rflags |= 0x100ULL; /* TF */
		else
			*rflags &= ~0x100ULL;
	}

	/* The snapshot as well, and at an exec stop INSTEAD: there is no frame
	 * there at all, because execve does not return through one -- it builds
	 * an IRET frame and jumps, taking its flags from here.  So all three
	 * resume paths are covered: an exception frame, a syscall frame, or the
	 * jump into a freshly loaded image. */
	if (on)
		t->syscall_rflags |= 0x100ULL;
	else
		t->syscall_rflags &= ~0x100ULL;
	return 0;
}

/* Move bytes to or from another process's memory.
 *
 * `mm' is the target's address-space owner (its thread-group leader) and the
 * caller must hold mm->mmap_lock -- shared to read, EXCLUSIVE to write, since
 * writing may have to break copy-on-write.  `kbuf' is kernel memory; user
 * memory is never touched from in here, because this runs under a sleeping
 * lock and a demand-paged fault would want to sleep again.
 *
 * Everything this refuses, and why:
 *
 *   - addresses outside the user range, and any length that carries the end
 *     out of it.  validate_user_ptr() bounds only the start.
 *   - an address with no region behind it: an unmapped hole reads as zero
 *     through the page tables and would look like real memory.
 *   - a region marked `device'.  Those are MMIO -- framebuffer aperture today,
 *     any BAR later -- and a write there is not a write to memory at all, it
 *     is driving hardware from another process's syscall.  Shared-memory
 *     attachments carry the same flag and are excluded with them.
 *   - a write to a region without PROT_WRITE.  Text is the interesting case:
 *     a debugger DOES plant breakpoints in it, so this is the one place the
 *     region's own protection is deliberately not the last word -- see the
 *     PROT_EXEC allowance below.
 *
 * Translation is per page because one translation covers exactly one page; a
 * request spanning a boundary is split rather than trusted to stay put.
 */
static int64_t ptrace_xfer_mem(task_t *mm, uint64_t addr, void *kbuf,
			       size_t len, bool write)
{
	uint8_t *b = (uint8_t *)kbuf;
	uint64_t end = addr + len;

	if (len == 0)
		return 0;
	if (!validate_user_ptr(addr, len))
		return -EFAULT;
	/* Re-bound the END.  validate_user_ptr checks the start against the top
	 * of the user half and checks that start+len does not wrap, but not
	 * that the end is still below the top. */
	if (end < addr || end > 0x7FFFFFFFFFFFULL)
		return -EFAULT;
	if (!mm->pml4 || mm->has_exited || mm->in_exit_teardown)
		return -ESRCH;

	while (len > 0) {
		uint64_t page = addr & ~0xFFFULL;
		size_t off = (size_t)(addr - page);
		size_t n = PAGE_SIZE - off;

		if (n > len)
			n = len;

		/* The PAGE TABLES are the authority on what is mapped here.
		 *
		 * A region record is not, and must not be treated as one: this
		 * kernel creates them only for lazily-mapped ranges and for
		 * mmap(), so an eagerly-loaded ELF segment has none and neither
		 * does the stack.  Requiring a region refused most of a normal
		 * process's memory -- every global it had already touched
		 * included -- while reporting it as a bad address. */
		/* Materialise the page first if it is only lazily mapped.
		 *
		 * A tracee stopped at its exec boundary has touched almost
		 * nothing, and it cannot fault its own pages in while it is
		 * stopped -- so without this, reading its code or planting a
		 * breakpoint in it fails for the whole of the program a
		 * debugger most wants to look at.  Its own regions decide what
		 * may be materialised; an address with nothing mapped behind it
		 * is still refused below. */
		(void)mm_populate_in(mm, page);

		uint64_t *pte =
			mm_get_page_table_from_pml4(mm->pml4, page, false);

		if (!pte || !(*pte & PAGE_PRESENT)) {
			/* Nothing mapped at this address, and nothing the region
			 * table would let us map there either.
			 *
			 * Reported quietly: a debugger probes addresses it is
			 * not sure about -- that is how it discovers what is
			 * mapped -- so a refusal here is ordinary, not a defect
			 * worth a kernel warning. */
			return -EFAULT;
		}

		/* Device memory is refused on the PTE flag rather than on the
		 * region, because the flag is what actually marks it and it is
		 * there whether or not a record exists.  A write would not be a
		 * write to memory at all: it would be driving hardware from
		 * another process's syscall. */
		if (*pte & PAGE_DEVICE)
			return -EACCES;

		if (write) {
			/* Where a region exists it carries the protection the
			 * mapping was made with, which the page tables alone
			 * cannot be read for -- a copy-on-write page is
			 * read-only in the tables and still perfectly writable.
			 * Text is allowed through deliberately: planting
			 * breakpoints is the main reason to write to a tracee,
			 * and text is never PROT_WRITE.  A read-only DATA
			 * mapping stays refused, which is what keeps a write
			 * out of a shared file-backed page.
			 *
			 * With no region the address belongs to the image or
			 * the stack, both private to this process, and the COW
			 * break below keeps it that way. */
			mmap_region_t *r = mm_find_mmap_region(mm, addr);

			if (r && !(r->prot & PROT_WRITE) &&
			    !(r->prot & PROT_EXEC))
				return -EACCES;

			/* Make the page privately writable BEFORE writing.
			 *
			 * Two things are being dealt with at once: a page still
			 * shared copy-on-write after a fork, where the write
			 * would otherwise land in the sibling's memory too, and
			 * a read-only mapping such as text, where it would not
			 * land at all.  Text is the ordinary case for a
			 * debugger -- a breakpoint is a write into code. */
			if (!mm_make_writable_in(mm->pml4, page))
				return -EFAULT;
		}

		uint64_t phys = mm_virt_to_phys_in(mm->pml4, addr);

		if (!phys) {
			/* Not materialised.  A lazily-mapped region has a
			 * record but no page until something touches it, and
			 * there is no way to fault one in on behalf of a task
			 * that is not running, so this is reported rather than
			 * guessed at. */
			return -EFAULT;
		}

		void *kaddr = phys_to_virt(phys);

		if (write)
			mm_memcpy(kaddr, b, n);
		else
			mm_memcpy(b, kaddr, n);

		addr += n;
		b += n;
		len -= n;
	}
	return 0;
}

/* Resolve a stopped tracee's address space for a memory transfer.
 *
 * Returns the mm owner with NO lock held, having verified under
 * g_task_list_lock that it is still this caller's tracee, still permitted, and
 * still has an address space.  The caller then takes mmap_lock -- which it
 * cannot do while holding a spinlock with interrupts off, since that lock
 * sleeps, which is the whole reason this is split in two.
 *
 * KNOWN LIMIT: nothing keeps the task_t itself alive across that gap.  There
 * is no refcount on tasks here, so this carries the same exposure as every
 * other cross-task operation in this kernel (sys_kill and the scheduler wakes
 * included).  Closing it properly needs task lifetime to be refcounted; doing
 * it locally without that would mean holding a spinlock across a sleeping
 * lock, which deadlocks. */
static task_t *ptrace_mem_target(task_t *cur, uint64_t pid, int64_t *err)
{
	uint64_t flags;
	task_t *mm = NULL;

	task_t *t = ptrace_lock_tracee(cur, pid, &flags, err);

	if (!t)
		return NULL;

	if (!t->ptrace_stopped) {
		/* Reading a running tracee gives a torn answer and writing one
		 * races whatever it is doing.  Both are refused. */
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		*err = -ESRCH;
		return NULL;
	}

	mm = task_mm_owner(t);
	if (!mm || !mm->pml4) {
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		*err = -ESRCH;
		return NULL;
	}
	spin_unlock_irqrestore(&g_task_list_lock, flags);
	return mm;
}

/* Clear the trace-stop state of a tracee that is about to be resumed.  The
 * caller does the waking; this only says the stop is over.  Runs with
 * g_task_list_lock held. */
static int64_t ptrace_resume(task_t *t)
{
	if (!t->ptrace_stopped)
		return -ESRCH; /* not stopped: nothing to resume */

	t->ptrace_stopped = 0;
	t->ptrace_stop_signo = 0;
	t->ptrace_event = 0;
	t->ptrace_group_stop = 0;

	/* The rest of the process goes too.  The siblings were parked by the
	 * group stop and carry no event of their own, so nothing else would
	 * ever release them: no waitpid reports them, and no PTRACE_CONT is
	 * aimed at them. */
	task_ptrace_group_resume(t);
	return 0;
}

int64_t sys_ptrace(uint64_t request, uint64_t pid, uint64_t addr,
		   uint64_t data)
{
	task_t *cur = sched_current();
	uint64_t flags;
	int64_t err = 0;

	if (!cur)
		return -EPERM;

	/* The one request that is about the caller itself and names no target:
	 * a process volunteering to be traced by its own parent.  No credential
	 * check applies -- consenting to be traced is not an act against
	 * anybody, and the parent could read this child's memory anyway. */
	if (request == PT_TRACEME) {
		int64_t r = 0;

		/* ->parent is read under the lock along with the write: an
		 * orphaned task is reparented from another context, so reading
		 * it outside and writing inside could record a parent that is
		 * no longer ours by the time it lands. */
		spin_lock_irqsave(&g_task_list_lock, &flags);
		{
			task_t *parent = cur->parent;

			if (cur->tracer_pid != 0)
				r = -EPERM; /* already traced */
			else if (!task_ptr_ok(parent) || parent->has_exited)
				r = -ESRCH;
			else {
				cur->tracer_pid = (int)parent->id;
				cur->tracer_incarnation = parent->incarnation;
			}
		}
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return r;
	}

	if (request == PT_ATTACH) {
		spin_lock_irqsave(&g_task_list_lock, &flags);

		task_t *t = sched_find_task_by_id_locked((uint32_t)pid);
		if (!t) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -ESRCH;
		}
		/* Tracing yourself, or a kernel thread, is not a thing that can
		 * work: the first deadlocks on its own stop, the second has no
		 * user context to stop in. */
		if (t == cur || t->privilege == TASK_KERNEL) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -EPERM;
		}
		if (t->tracer_pid != 0) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -EBUSY; /* somebody is already tracing it */
		}

		/* The full attach rule: matching uid AND gid triples across the
		 * target's whole thread group, and a refusal for a process
		 * whose privileges came from a set-id exec.  Root bypasses. */
		int perm = task_may_access(t, ACCESS_ATTACH);
		if (perm != 0) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return perm;
		}

		/* Recorded as the PROCESS, matching PTRACE_TRACEME and what
		 * ptrace_lock_tracee() checks against. */
		task_t *tracer = cur->group_leader ? cur->group_leader : cur;

		/* The whole thread group, not just the thread named.
		 *
		 * A debugger attaches to a PROCESS.  Tracing only the thread
		 * whose id happened to be passed leaves the others running
		 * untraced: their registers cannot be read, a fault on one is
		 * reported to nobody, and `info threads' shows a single stack
		 * for a program doing its work on several.  Every thread here
		 * is a task with its own registers and its own stop, so tracing
		 * all of them is what makes them debuggable.
		 *
		 * Walked with the same guard as the other group walks: a ring
		 * that does not lead back to its leader must not be followed
		 * into whatever it points at with interrupts off. */
		{
			task_t *leader = t->group_leader ? t->group_leader : t;
			task_t *w = leader;
			int guard = 0;

			if (task_ptr_ok(leader)) {
				do {
					if (!w->has_exited &&
					    w->privilege != TASK_KERNEL &&
					    w->tracer_pid == 0) {
						w->tracer_pid =
							(int)tracer->id;
						w->tracer_incarnation =
							tracer->incarnation;
					}
					w = w->thread_group_next;
					if (!task_ptr_ok(w) ||
					    ++guard > TASK_GROUP_KILL_MAX * 4) {
						WARN_ON_ONCE(1);
						break;
					}
				} while (w != leader);
			} else {
				t->tracer_pid = (int)tracer->id;
				t->tracer_incarnation = tracer->incarnation;
			}
		}
		spin_unlock_irqrestore(&g_task_list_lock, flags);

		/* Stop it so the tracer has something to wait for, the way an
		 * attach is expected to behave.  Delivered as a signal rather
		 * than parked directly: the tracee has to reach its own context
		 * to stop, and the stop redirect in the signal path is what
		 * turns this into a trace stop. */
		sched_signal_task(t, SIGSTOP);
		return 0;
	}

	/* Everything else names a tracee this task is already tracing. */
	task_t *t = ptrace_lock_tracee(cur, pid, &flags, &err);
	if (!t)
		return err;

	int64_t ret = 0;

	switch (request) {
	case PT_DETACH: {
		int signo = (int)data;

		/* Same gate as PT_CONT: a signal requested here is still a
		 * signal being sent, and must pass the check that applies to
		 * sending one. */
		if (signo != 0) {
			int64_t perm = signal_target_check(t, signo);

			if (perm != 0) {
				spin_unlock_irqrestore(&g_task_list_lock,
						       flags);
				return perm;
			}
		}

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		/* Takes the lock itself, and enqueues, so it cannot run with
		 * the task list held. */
		task_ptrace_detach(t);
		if (signo != 0)
			sched_signal_task(t, signo);
		return 0;
	}

	case PT_CONT: {
		int signo = (int)data;

		/* A signal asked for on resume goes through the same gate as
		 * kill(2).  The tracer is asking for a signal to be sent, and
		 * being a tracer is not a way around the check that applies to
		 * sending one -- sched_signal_task() and signal_send() perform
		 * no credential check of their own. */
		if (signo != 0) {
			int64_t perm = signal_target_check(t, signo);

			if (perm != 0) {
				spin_unlock_irqrestore(&g_task_list_lock,
						       flags);
				return perm;
			}
		}

		/* A plain continue is not a syscall-traced one.  Clearing
		 * this is what lets a tracer stop asking: it describes this
		 * resume, not the trace as a whole. */
		t->ptrace_syscall_trace = 0;
		t->ptrace_in_syscall = 0;

		/* Marked, not withheld: the signal is SENT below by the same
		 * path that worked before signal-delivery-stop existed.  The
		 * mark tells the delivery stop to let this one through, rather
		 * than stopping to ask about the signal the tracer just asked
		 * to deliver -- which would never terminate. */
		if (signo != 0)
			t->ptrace_signal_injected = 1;

		ret = ptrace_resume(t);
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		if (ret != 0)
			return ret;

		/* Queue the signal BEFORE making the tracee runnable.
		 *
		 * The other order is a race, and a losing one: the tracee wakes
		 * on another CPU, comes out of its signal-delivery stop, looks
		 * for a signal to act on and finds the queue still empty
		 * because this call has not sent it yet.  It then carries on --
		 * and if its next act is _exit(), the signal the tracer asked
		 * to deliver is never delivered at all.  Sending first costs
		 * nothing: a stopped task simply has the signal waiting when it
		 * runs. */
		if (signo != 0)
			sched_signal_task(t, signo);
		if (sched_claim_wake(t, TASK_STOPPED))
			sched_enqueue_ready(t);
		return 0;
	}

	case PT_KILL: {
		/* Routed through the ordinary signal gate rather than killing
		 * directly: sched_signal_task() and signal_send() apply no
		 * credential check of their own, so a ptrace path that reached
		 * for them straight would be a way to kill a process the caller
		 * has no right to signal.  The check only reads credentials, so
		 * it runs under the lock; the delivery must not. */
		int64_t perm = signal_target_check(t, SIGKILL);

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		if (perm != 0)
			return perm;
		sched_signal_task(t, SIGKILL);
		return 0;
	}

	case PT_SETOPTIONS: {
		if (data & ~(uint64_t)PTRACE_O_ALL) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -EINVAL;
		}

		/* Set on the whole thread group, like the attach that named it.
		 *
		 * The options are asked for once, against the process, and a
		 * debugger has no reason to name each thread -- so setting them
		 * only on the thread whose id was passed left every OTHER
		 * thread of an attached process reporting nothing: a thread it
		 * created was never announced, a thread that died was never
		 * reported.  New threads inherit these from their creator (see
		 * sys_clone), so this is what makes the whole process, present
		 * threads and future ones alike, trace on the terms asked for.
		 *
		 * Walked with the guard the other group walks use: a ring that
		 * does not lead back to its leader must not be followed into
		 * whatever it points at with interrupts off. */
		task_t *leader = t->group_leader ? t->group_leader : t;
		task_t *w = leader;
		int guard = 0;

		t->ptrace_options = (uint32_t)data;

		if (task_ptr_ok(leader)) {
			do {
				/* Only the threads this caller actually traces.
				 * tracer_pid is what says so, and it is the
				 * same test every other request applies. */
				if (!w->has_exited &&
				    w->tracer_pid == t->tracer_pid &&
				    w->tracer_incarnation ==
					    t->tracer_incarnation)
					w->ptrace_options = (uint32_t)data;
				w = w->thread_group_next;
				if (!task_ptr_ok(w) ||
				    ++guard > TASK_GROUP_KILL_MAX * 4) {
					WARN_ON_ONCE(1);
					break;
				}
			} while (w != leader);
		}

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return 0;
	}

	case PT_GETEVENTMSG: {
		unsigned long msg = t->ptrace_msg;
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		if (!validate_user_ptr(data, sizeof(unsigned long)))
			return -EFAULT;
		if (copy_to_user((void *)data, &msg, sizeof(msg)) < 0)
			return -EFAULT;
		return 0;
	}

	/*
	 * Copy out the tracee's auxiliary vector: ADDR is the tracer's buffer,
	 * DATA its size, and the return value is the number of bytes copied.
	 *
	 * The block lives on the tracee's stack, so this is a cross-address-
	 * space read and goes through the same hardened path as PEEKDATA
	 * rather than touching the other process's pages directly.  Its
	 * location is read under the task-list lock, which is dropped before
	 * any of the work that can sleep.
	 */
	case PT_GETAUXV: {
		uint64_t src = t->auxv_addr;
		uint64_t srclen = t->auxv_len;

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		/* Never exec'd, so there is no vector to report.  Not an
		 * error: a tracee stopped at PTRACE_TRACEME before its exec is
		 * a legitimate thing to ask about, and the honest answer is
		 * that it has none yet. */
		if (!src || !srclen)
			return 0;

		if (srclen > data)
			srclen = data;
		if (srclen == 0)
			return 0;
		if (!validate_user_ptr(addr, srclen))
			return -EFAULT;

		void *kbuf = kalloc(srclen);
		if (!kbuf)
			return -ENOMEM;

		task_t *mm = ptrace_mem_target(cur, pid, &err);
		if (!mm) {
			kfree(kbuf);
			return err;
		}

		mm_read_lock(&mm->mmap_lock);
		ret = ptrace_xfer_mem(mm, src, kbuf, srclen, false);
		mm_read_unlock(&mm->mmap_lock);
		if (ret != 0) {
			kfree(kbuf);
			return ret;
		}

		if (copy_to_user((void *)addr, kbuf, srclen) < 0) {
			kfree(kbuf);
			return -EFAULT;
		}
		kfree(kbuf);
		return (int64_t)srclen;
	}

	/* The register requests take their buffer in ADDR, not in DATA.
	 *
	 * That is the convention gdb's own generic ptrace target already
	 * speaks, and matching it is what lets this system reuse
	 * inf-ptrace.c and amd64-bsd-nat.c as they are rather than carry a
	 * hand-written native module.  Every other request here was already
	 * compatible -- a resume passes its signal in DATA and ignores ADDR,
	 * exactly as gdb sends it -- so these four were the whole difference.
	 */
	case PT_GETREGS: {
		ptrace_regs_t r;

		if (!t->ptrace_stopped) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -ESRCH;
		}
		ptrace_read_regs(t, &r);
		spin_unlock_irqrestore(&g_task_list_lock, flags);

		if (!validate_user_ptr(addr, sizeof(r)))
			return -EFAULT;
		if (copy_to_user((void *)addr, &r, sizeof(r)) < 0)
			return -EFAULT;
		return 0;
	}

	case PT_SETREGS: {
		ptrace_regs_t r;

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		if (!validate_user_ptr(addr, sizeof(r)))
			return -EFAULT;
		if (copy_from_user(&r, (void *)addr, sizeof(r)) < 0)
			return -EFAULT;

		/* Re-resolve: the copy above may sleep on a demand-paged user
		 * page, and the tracee's state can change while it does. */
		t = ptrace_lock_tracee(cur, pid, &flags, &err);
		if (!t)
			return err;
		if (!t->ptrace_stopped) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -ESRCH;
		}
		ret = ptrace_write_regs(t, &r);
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return ret;
	}

	case PT_GETFPREGS:
	case PT_SETFPREGS: {
		/* The x87/SSE file already has a per-task home: the FXSAVE area
		 * the context switch keeps.  Nothing new to save, only a copy
		 * in or out of it. */
		uint8_t *area = t->fpu_state;

		if (!t->ptrace_stopped || !area) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -ESRCH;
		}

		uint8_t tmp[512];

		if (request == PT_GETFPREGS) {
			mm_memcpy(tmp, area, sizeof(tmp));
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			if (!validate_user_ptr(addr, sizeof(tmp)))
				return -EFAULT;
			if (copy_to_user((void *)addr, tmp, sizeof(tmp)) < 0)
				return -EFAULT;
			return 0;
		}

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		if (!validate_user_ptr(addr, sizeof(tmp)))
			return -EFAULT;
		if (copy_from_user(tmp, (void *)addr, sizeof(tmp)) < 0)
			return -EFAULT;

		t = ptrace_lock_tracee(cur, pid, &flags, &err);
		if (!t)
			return err;
		if (!t->ptrace_stopped || !t->fpu_state) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return -ESRCH;
		}
		mm_memcpy(t->fpu_state, tmp, sizeof(tmp));
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return 0;
	}

	case PT_PEEKTEXT:
	case PT_PEEKDATA: {
		uint64_t word = 0;

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		task_t *mm = ptrace_mem_target(cur, pid, &err);

		if (!mm)
			return err;

		mm_read_lock(&mm->mmap_lock);
		ret = ptrace_xfer_mem(mm, addr, &word, sizeof(word), false);
		mm_read_unlock(&mm->mmap_lock);
		if (ret != 0)
			return ret;

		/* The word is returned, not written through `data', so that a
		 * PEEK costs one syscall.  See the note in the libc wrapper
		 * about telling a read of -1 from a failure. */
		return (int64_t)word;
	}

	case PT_POKETEXT:
	case PT_POKEDATA: {
		uint64_t word = data;

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		task_t *mm = ptrace_mem_target(cur, pid, &err);

		if (!mm)
			return err;

		/* Exclusive: a write may have to break copy-on-write, which
		 * rewrites a page-table entry. */
		mm_write_lock(&mm->mmap_lock);
		ret = ptrace_xfer_mem(mm, addr, &word, sizeof(word), true);
		mm_write_unlock(&mm->mmap_lock);
		return ret;
	}

	case PT_SYSCALL: {
		int signo = (int)data;

		if (signo != 0) {
			int64_t perm = signal_target_check(t, signo);

			if (perm != 0) {
				spin_unlock_irqrestore(&g_task_list_lock,
						       flags);
				return perm;
			}
		}

		/* Run until the next syscall boundary.  Set per resume, not as
		 * an option, so alternating PTRACE_SYSCALL and PTRACE_CONT does
		 * what it says. */
		t->ptrace_syscall_trace = 1;

		/* Marked, not withheld: the signal is SENT below by the same
		 * path that worked before signal-delivery-stop existed.  The
		 * mark tells the delivery stop to let this one through, rather
		 * than stopping to ask about the signal the tracer just asked
		 * to deliver -- which would never terminate. */
		if (signo != 0)
			t->ptrace_signal_injected = 1;

		ret = ptrace_resume(t);
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		if (ret != 0)
			return ret;

		/* Queue the signal BEFORE making the tracee runnable.
		 *
		 * The other order is a race, and a losing one: the tracee wakes
		 * on another CPU, comes out of its signal-delivery stop, looks
		 * for a signal to act on and finds the queue still empty
		 * because this call has not sent it yet.  It then carries on --
		 * and if its next act is _exit(), the signal the tracer asked
		 * to deliver is never delivered at all.  Sending first costs
		 * nothing: a stopped task simply has the signal waiting when it
		 * runs. */
		if (signo != 0)
			sched_signal_task(t, signo);
		if (sched_claim_wake(t, TASK_STOPPED))
			sched_enqueue_ready(t);
		return 0;
	}

	case PT_SINGLESTEP: {
		int signo = (int)data;

		if (signo != 0) {
			int64_t perm = signal_target_check(t, signo);

			if (perm != 0) {
				spin_unlock_irqrestore(&g_task_list_lock,
						       flags);
				return perm;
			}
		}

		/* Arm the trap flag before releasing the stop: the processor
		 * raises #DB after the next instruction, which the exception
		 * handler turns back into a stop for this tracer. */
		t->ptrace_syscall_trace = 0;
		ret = ptrace_set_tf(t, true);
		if (ret != 0) {
			spin_unlock_irqrestore(&g_task_list_lock, flags);
			return ret;
		}

		/* Marked, not withheld: the signal is SENT below by the same
		 * path that worked before signal-delivery-stop existed.  The
		 * mark tells the delivery stop to let this one through, rather
		 * than stopping to ask about the signal the tracer just asked
		 * to deliver -- which would never terminate. */
		if (signo != 0)
			t->ptrace_signal_injected = 1;

		ret = ptrace_resume(t);
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		if (ret != 0)
			return ret;

		/* Queue the signal BEFORE making the tracee runnable.
		 *
		 * The other order is a race, and a losing one: the tracee wakes
		 * on another CPU, comes out of its signal-delivery stop, looks
		 * for a signal to act on and finds the queue still empty
		 * because this call has not sent it yet.  It then carries on --
		 * and if its next act is _exit(), the signal the tracer asked
		 * to deliver is never delivered at all.  Sending first costs
		 * nothing: a stopped task simply has the signal waiting when it
		 * runs. */
		if (signo != 0)
			sched_signal_task(t, signo);
		if (sched_claim_wake(t, TASK_STOPPED))
			sched_enqueue_ready(t);
		return 0;
	}

	/* Syscall entry/exit stops and the debug registers arrive with the
	 * phases that build them; refused outright rather than silently doing
	 * nothing, so a debugger gets a straight answer instead of a tracee
	 * that never stops. */
	/*
	 * The threads of the tracee's process.
	 *
	 * A debugger has to know they exist before it can do anything with
	 * them: without this it sees one thread, reports one stack, and a
	 * program that does its work on other threads looks idle.  The kernel
	 * is the only thing that knows the set, since a thread is not visible
	 * from outside the process.
	 *
	 * The two requests are the conventional pair: ask how many, then ask
	 * for that many.  Both walk the group ring under the task-list lock,
	 * with the guard the other group walks use -- a ring that does not lead
	 * back to its leader would otherwise be followed into whatever it
	 * points at, with interrupts off, where a fault takes the machine down
	 * rather than the process.
	 */
	case PT_GETNUMLWPS: {
		task_t *leader = t->group_leader ? t->group_leader : t;
		int count = 0;

		if (task_ptr_ok(leader)) {
			task_t *w = leader;
			int guard = 0;

			do {
				if (!w->has_exited)
					count++;
				w = w->thread_group_next;
				if (!task_ptr_ok(w) ||
				    ++guard > TASK_GROUP_KILL_MAX * 4) {
					WARN_ON_ONCE(1);
					break;
				}
			} while (w != leader);
		}

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return count;
	}

	case PT_GETLWPLIST: {
		/* ADDR is the buffer, DATA how many ids fit in it.  Returns how
		 * many were written, which may be fewer than exist -- the
		 * caller sized the buffer from GETNUMLWPS and threads can come
		 * and go in between. */
		uint32_t ids[TASK_GROUP_KILL_MAX];
		task_t *leader = t->group_leader ? t->group_leader : t;
		int count = 0;
		uint64_t want = data;

		if (task_ptr_ok(leader)) {
			task_t *w = leader;
			int guard = 0;

			do {
				if (!w->has_exited &&
				    count < TASK_GROUP_KILL_MAX)
					ids[count++] = (uint32_t)w->id;
				w = w->thread_group_next;
				if (!task_ptr_ok(w) ||
				    ++guard > TASK_GROUP_KILL_MAX * 4) {
					WARN_ON_ONCE(1);
					break;
				}
			} while (w != leader);
		}

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		if ((uint64_t)count > want)
			count = (int)want;
		if (count <= 0)
			return 0;
		if (!validate_user_ptr(addr, (uint64_t)count * sizeof(ids[0])))
			return -EFAULT;
		if (copy_to_user((void *)addr, ids,
				 (size_t)count * sizeof(ids[0])) < 0)
			return -EFAULT;
		return count;
	}

	/*
	 * Which EVENT the tracee is stopped for, if any.
	 *
	 * The stop already reports a signal, and for the event stops that
	 * signal is SIGTRAP -- indistinguishable from a breakpoint unless the
	 * event code is asked for.  A debugger needs the distinction to follow
	 * a fork, to notice a new thread, or to know that an exec has replaced
	 * the program it was debugging.
	 *
	 * The value that came with it (a new child's or thread's pid) is
	 * PTRACE_GETEVENTMSG; this is the code that says what the value means.
	 */
	case PT_GETEVENT: {
		int event = t->ptrace_event;

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return event;
	}

	/*
	 * The tracee's executable, as an absolute path.  ADDR is the buffer,
	 * DATA its size; the return value is the length copied, not counting
	 * the terminator.  Zero means the task has never exec'd (a kernel
	 * thread, or a child stopped at PTRACE_TRACEME before its exec) --
	 * which is an answer, not an error.
	 *
	 * A debugger that ATTACHED has no other way to find the binary.  `comm'
	 * is a basename and argv[0] is whatever the program was called with --
	 * a login shell's is "-bash", which opens nothing -- so without this a
	 * `gdb -p' session has no symbols, cannot place a breakpoint by name,
	 * and cannot relocate a position-independent image.  Systems with a
	 * per-process filesystem publish the same string as /proc/PID/exe.
	 */
	case PT_GETEXECPATH: {
		char path[256];
		size_t len = 0;

		while (len < sizeof(path) - 1 && t->exe_path[len]) {
			path[len] = t->exe_path[len];
			len++;
		}
		path[len] = '\0';

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		if (len == 0)
			return 0;
		/* The terminator has to fit too: a caller given a truncated
		 * path with no NUL would read past its own buffer. */
		if (data < (uint64_t)len + 1)
			return -ERANGE;
		if (!validate_user_ptr(addr, len + 1))
			return -EFAULT;
		if (copy_to_user((void *)addr, path, len + 1) < 0)
			return -EFAULT;
		return (int64_t)len;
	}

	case PT_GETSIGINFO: {
		siginfo_t si = t->ptrace_siginfo;

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		if (!validate_user_ptr(addr, sizeof(si)))
			return -EFAULT;
		if (copy_to_user((void *)addr, &si, sizeof(si)) < 0)
			return -EFAULT;
		return 0;
	}

	case PT_GETDBREGS: {
		uint64_t dbr[8];

		dbr[0] = t->dr_addr[0];
		dbr[1] = t->dr_addr[1];
		dbr[2] = t->dr_addr[2];
		dbr[3] = t->dr_addr[3];
		/* 4 and 5 alias 6 and 7 on the hardware; reported as zero
		 * rather than aliased, so nothing reads them by accident. */
		dbr[4] = 0;
		dbr[5] = 0;
		dbr[6] = t->dr_status;
		dbr[7] = t->dr_control;

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		if (!validate_user_ptr(addr, sizeof(dbr)))
			return -EFAULT;
		if (copy_to_user((void *)addr, dbr, sizeof(dbr)) < 0)
			return -EFAULT;
		return 0;
	}

	case PT_SETDBREGS: {
		uint64_t dbr[8];

		spin_unlock_irqrestore(&g_task_list_lock, flags);

		if (!validate_user_ptr(addr, sizeof(dbr)))
			return -EFAULT;
		if (copy_from_user(dbr, (const void *)addr, sizeof(dbr)) < 0)
			return -EFAULT;

		/* A watchpoint address is compared against every access the
		 * processor makes, the kernel's own included, so an address
		 * outside this task's own user space is refused outright
		 * rather than trimmed.  Trimming would arm a watchpoint on
		 * something the tracer did not ask for. */
		for (int i = 0; i < 4; i++) {
			if (dbr[i] == 0)
				continue;
			if (!validate_user_ptr(dbr[i], 1))
				return -EINVAL;
		}

		uint64_t dr7 = ptrace_sanitize_dr7(dbr[7]);

		/* Re-resolve: the lock was dropped for the copies above, which
		 * can sleep, and the tracee could have gone in the meantime.
		 * The access check runs again with it, per the rule that
		 * permission is never cached across a request. */
		t = ptrace_lock_tracee(cur, pid, &flags, &err);
		if (!t)
			return err;

		for (int i = 0; i < 4; i++)
			t->dr_addr[i] = dbr[i];
		t->dr_control = dr7;
		/* DR6 is the hardware's to report, not the tracer's to set;
		 * accepting a value would let a debugger fake which watchpoint
		 * fired.  Writing any DR6 clears the stale status instead. */
		t->dr_status = 0;
		t->dr_active = (dr7 & DR7_ENABLE_MASK) ? 1 : 0;

		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return 0;
	}

	case PT_PEEKUSER:
	case PT_POKEUSER:
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return -ENOSYS;

	default:
		spin_unlock_irqrestore(&g_task_list_lock, flags);
		return -EINVAL;
	}
}
