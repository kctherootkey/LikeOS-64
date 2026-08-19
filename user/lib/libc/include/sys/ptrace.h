/* sys/ptrace.h — process tracing.
 *
 * ptrace() lets one process observe and control another: read and write its
 * memory and registers, stop it, step it one instruction at a time, and be
 * told when it stops.  It is what a debugger is built on.
 *
 * This is LikeOS's own interface rather than a copy of any other system's.
 * Every Unix that has ptrace numbers the requests differently and describes
 * registers differently, and debuggers carry a per-system module for exactly
 * that reason, so there is nothing to be gained by pretending to be one of
 * them and a correctness trap in pretending badly.
 *
 * ACCESS CONTROL.  Tracing hands over complete control of a process, so the
 * kernel applies the strict rule to every request, not just to the attach:
 *
 *   - the caller's effective uid must match the target's real, effective AND
 *     saved uids, and likewise for the group ids;
 *   - a process whose privileges were raised by a set-id exec is refused
 *     outright to an unprivileged caller, because it holds what that identity
 *     could reach and its owner's could not;
 *   - the answer is computed across the target's whole thread group, so a
 *     process cannot be reached through whichever of its threads happens to
 *     hold the weakest credentials;
 *   - root (effective uid 0) may trace anything.
 *
 * It is re-evaluated on every request rather than cached at attach: a tracee
 * can change identity while it is being traced, and a decision made before
 * that would be the wrong one afterwards.
 *
 * A traced process also does not gain privilege from a set-id exec while an
 * unprivileged tracer is attached -- the exec still runs, it just does not
 * raise the ids.  Otherwise attaching to a program that is about to run a
 * set-id binary would be a way to inherit its identity.
 */
#ifndef _SYS_PTRACE_H
#define _SYS_PTRACE_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Requests.  The numbering is this system's own; see the note above. */
enum {
	/* Declare that this process is to be traced by its parent.  The only
	 * request a process makes about itself, and the only one that does not
	 * name a pid. */
	PTRACE_TRACEME = 0,

	/* Read/write one word of the tracee's memory.  TEXT and DATA are the
	 * same address space here and the two names are kept only because
	 * every debugger already says both. */
	PTRACE_PEEKTEXT = 1,
	PTRACE_PEEKDATA = 2,
	PTRACE_POKETEXT = 3,
	PTRACE_POKEDATA = 4,

	/* Whole register sets, general-purpose and floating-point. */
	PTRACE_GETREGS = 5,
	PTRACE_SETREGS = 6,
	PTRACE_GETFPREGS = 7,
	PTRACE_SETFPREGS = 8,

	/* Resume a stopped tracee.  `data' is a signal number to deliver as it
	 * resumes, or 0 for none.  SINGLESTEP resumes for exactly one
	 * instruction; SYSCALL resumes until the next syscall boundary. */
	PTRACE_CONT = 9,
	PTRACE_SINGLESTEP = 10,
	PTRACE_SYSCALL = 11,

	/* Stop tracing.  DETACH leaves the tracee running. */
	PTRACE_ATTACH = 12,
	PTRACE_DETACH = 13,

	/* Kill the tracee. */
	PTRACE_KILL = 14,

	/* The debug registers, by offset -- hardware breakpoints and
	 * watchpoints.  They are privileged registers, so the kernel does the
	 * actual write; these only say which one and what value. */
	PTRACE_PEEKUSER = 15,
	PTRACE_POKEUSER = 16,

	/* Trace options; see the PTRACE_O_* bits. */
	PTRACE_SETOPTIONS = 17,

	/* The value that accompanied the event a tracee last stopped for. */
	PTRACE_GETEVENTMSG = 18,

	/*
	 * Copy the tracee's auxiliary vector out.  ADDR is the destination
	 * buffer, DATA its size in bytes; the return value is how many bytes
	 * were copied, which is zero for a task that has never exec'd.
	 *
	 * A debugger cannot get far without this.  Executables here are
	 * position-independent, so every address in the symbol table is an
	 * offset until the bias the image was loaded at is known, and
	 * comparing AT_ENTRY in this vector against e_entry in the file on
	 * disk is how that bias is worked out.  Reading the loader's
	 * rendezvous instead would not help: finding it means reading the
	 * dynamic section, which means already knowing the bias.
	 *
	 * Systems that expose a per-process filesystem publish the same block
	 * as a file and have no request for it; this one has no such
	 * filesystem, so it is asked for directly.
	 */
	PTRACE_GETAUXV = 19,

	/*
	 * The hardware debug registers, in ADDR as a struct ptrace_dbregs.
	 *
	 * These are what make a watchpoint cost nothing while it is armed: the
	 * processor compares each memory access against the four addresses
	 * itself, where the alternative is single-stepping the whole program
	 * and re-reading the watched location after every instruction.
	 *
	 * What is written back is filtered, not stored verbatim -- see the
	 * kernel's ptrace_set_dbregs.  A debug register can stop the CPU on a
	 * kernel address, and DR7's GD bit stops it on any access to the debug
	 * registers at all, including the kernel's own; a tracer does not get
	 * to ask for either.  SETDBREGS therefore succeeds having quietly
	 * cleared bits it will not honour, and GETDBREGS reads back what was
	 * actually kept.
	 */
	PTRACE_GETDBREGS = 20,
	PTRACE_SETDBREGS = 21,

	/*
	 * The siginfo of the signal a tracee is stopped for; ADDR is a
	 * `siginfo_t *'.
	 *
	 * A debugger asks at every signal stop.  The signal number alone says
	 * a fault happened; si_code and si_addr say which kind and where --
	 * "SIGSEGV, address not mapped, 0x1" rather than "SIGSEGV".  gdb
	 * surfaces it as $_siginfo and in the line it prints when the inferior
	 * stops.
	 */
	PTRACE_GETSIGINFO = 22,

	/*
	 * The threads of the tracee's process: how many, and their ids.
	 *
	 * GETNUMLWPS returns the count.  GETLWPLIST takes the buffer in ADDR
	 * and its capacity in ids in DATA, and returns how many it wrote --
	 * possibly fewer than exist, since threads come and go between the two
	 * calls.
	 *
	 * A debugger needs them to show more than one stack.  Every thread is
	 * a task in its own right here, with its own registers and its own
	 * stop, so once they are named the rest of ptrace already works on
	 * them.
	 */
	PTRACE_GETNUMLWPS = 23,
	PTRACE_GETLWPLIST = 24,

	/*
	 * Which PTRACE_EVENT_* the tracee is stopped for, or 0 for an ordinary
	 * stop.  The stop's signal is SIGTRAP for all of them, so without this
	 * a fork, a new thread and a breakpoint are indistinguishable.
	 * PTRACE_GETEVENTMSG carries the value that goes with the event.
	 */
	PTRACE_GETEVENT = 25,

	/*
	 * The tracee's executable, as an absolute path.  ADDR is the buffer,
	 * DATA its size in bytes; the return value is the length written, not
	 * counting the terminator, and 0 for a task that has never exec'd.
	 * ERANGE if the buffer cannot hold the path and its terminator.
	 *
	 * A debugger attaching to a running process cannot find the binary any
	 * other way.  The process name is only a basename and argv[0] is
	 * whatever the program was invoked as -- a login shell's is "-bash",
	 * which names no file at all -- so without this an attached session has
	 * no symbols and no way to relocate a position-independent image.
	 * Systems with a per-process filesystem publish the same string as
	 * /proc/PID/exe and need no request for it.
	 */
	PTRACE_GETEXECPATH = 26,
};

/*
 * The debug registers, indexed as the hardware numbers them.
 *
 * dr[0] through dr[3] are the watchpoint addresses.  dr[4] and dr[5] are the
 * architecture's own aliases of dr[6] and dr[7] and are reported as zero.
 * dr[6] is the status word -- bits B0..B3 say which address matched, BS that a
 * single step completed -- and dr[7] the control word, which enables each
 * address and says whether it watches a read, a write, or an execution, and
 * over how many bytes.
 *
 * The array is kept the shape the hardware defines, holes included, because
 * every debugger's own tables are indexed that way.
 */
struct ptrace_dbregs {
	unsigned long dr[8];
};

/* Options for PTRACE_SETOPTIONS: which extra events should stop the tracee.
 * Without these a tracer only sees signal-delivery stops. */
#define PTRACE_O_TRACEFORK 0x0001
#define PTRACE_O_TRACEVFORK 0x0002
#define PTRACE_O_TRACECLONE 0x0004
#define PTRACE_O_TRACEEXEC 0x0008
#define PTRACE_O_TRACEEXIT 0x0010
/* Kill the tracee if the tracer goes away, rather than leaving it stopped for
 * ever with nobody to resume it. */
#define PTRACE_O_EXITKILL 0x0020

/* Why a tracee stopped, reported in the upper bits of the wait status. */
#define PTRACE_EVENT_FORK 1
#define PTRACE_EVENT_VFORK 2
#define PTRACE_EVENT_CLONE 3
#define PTRACE_EVENT_EXEC 4
#define PTRACE_EVENT_EXIT 5
/* Syscall entry and exit are distinguished explicitly.  Systems that report
 * both identically leave the tracer counting stops to tell them apart, which
 * desynchronises permanently the first time one is missed.  For FORK/VFORK/
 * CLONE, PTRACE_GETEVENTMSG returns the new process's pid. */
#define PTRACE_EVENT_SYSCALL_ENTRY 6
#define PTRACE_EVENT_SYSCALL_EXIT 7

/* The tracee's general-purpose registers, in the layout PTRACE_GETREGS and
 * PTRACE_SETREGS use.  Laid out to match the frame the kernel already keeps
 * for an interrupted or trapped task rather than to match anyone else's
 * struct. */
struct ptrace_regs {
	unsigned long r15, r14, r13, r12;
	unsigned long rbp, rbx;
	unsigned long r11, r10, r9, r8;
	unsigned long rax, rcx, rdx, rsi, rdi;
	unsigned long rip, cs, rflags, rsp, ss;
	/* The FS and GS bases, which are per-task state the kernel keeps: FS's
	 * is the thread pointer, and without it thread-local variables cannot
	 * be located.  Both are writable through PTRACE_SETREGS, and both are
	 * refused above the user half of the address space. */
	unsigned long fs_base, gs_base;
	/* The data-segment selectors, as the processor held them when the
	 * tracee last entered the kernel.  Reported for completeness and read
	 * only: in 64-bit mode a data selector selects nothing, the kernel does
	 * not restore these on the way out, and a value accepted here would
	 * therefore be one that never took effect.  Zero for a task that has
	 * not been to user mode.  Appended after the bases so that adding them
	 * did not move any register a debugger already reads. */
	unsigned long ds, es, fs, gs;
};

/* Pinned to the same size as the kernel's copy of this layout, which lives in
 * a different build and so cannot be compared against it directly.  Adding a
 * field to one and forgetting the other then fails to compile rather than
 * quietly shifting every register reported. */
#ifdef __cplusplus
static_assert(sizeof(struct ptrace_regs) == 26 * sizeof(unsigned long),
	      "struct ptrace_regs layout must match the kernel's");
#else
_Static_assert(sizeof(struct ptrace_regs) == 26 * sizeof(unsigned long),
	       "struct ptrace_regs layout must match the kernel's");
#endif

/* The x87/SSE state, as FXSAVE writes it. */
struct ptrace_fpregs {
	unsigned char area[512];
};

/* Debug-register offsets for PTRACE_PEEKUSER / PTRACE_POKEUSER.  DR0-DR3 hold
 * the addresses being watched, DR6 reports which fired, DR7 says what each one
 * watches and how wide.  DR4 and DR5 do not exist. */
#define PTRACE_DR_OFFSET(n) ((unsigned long)(n))
#define PTRACE_DR0 PTRACE_DR_OFFSET(0)
#define PTRACE_DR1 PTRACE_DR_OFFSET(1)
#define PTRACE_DR2 PTRACE_DR_OFFSET(2)
#define PTRACE_DR3 PTRACE_DR_OFFSET(3)
#define PTRACE_DR6 PTRACE_DR_OFFSET(6)
#define PTRACE_DR7 PTRACE_DR_OFFSET(7)

/* ---- Traditional request names ---------------------------------------------
 *
 * The same requests under the names the BSD-derived interface uses.  They exist
 * because that is the spelling debuggers' portable ptrace code is written
 * against -- gdb's generic native target among them -- so providing them lets
 * such code compile here unmodified instead of needing a port of its own.
 *
 * Same numbers, same arguments: these are aliases, not a second interface. */
#define PT_TRACE_ME PTRACE_TRACEME
#define PT_READ_I PTRACE_PEEKTEXT
#define PT_READ_D PTRACE_PEEKDATA
#define PT_WRITE_I PTRACE_POKETEXT
#define PT_WRITE_D PTRACE_POKEDATA
#define PT_GETREGS PTRACE_GETREGS
#define PT_SETREGS PTRACE_SETREGS
#define PT_GETFPREGS PTRACE_GETFPREGS
#define PT_SETFPREGS PTRACE_SETFPREGS
#define PT_CONTINUE PTRACE_CONT
#define PT_STEP PTRACE_SINGLESTEP
#define PT_SYSCALL PTRACE_SYSCALL
#define PT_ATTACH PTRACE_ATTACH
#define PT_DETACH PTRACE_DETACH
#define PT_KILL PTRACE_KILL
#define PT_GETEVENTMSG PTRACE_GETEVENTMSG
#define PT_GETAUXV PTRACE_GETAUXV
#define PT_GETDBREGS PTRACE_GETDBREGS
#define PT_SETDBREGS PTRACE_SETDBREGS
#define PT_GETSIGINFO PTRACE_GETSIGINFO
#define PT_GETNUMLWPS PTRACE_GETNUMLWPS
#define PT_GETLWPLIST PTRACE_GETLWPLIST
#define PT_GETEVENT PTRACE_GETEVENT
#define PT_GETEXECPATH PTRACE_GETEXECPATH

/* Trace a process.
 *
 * `request' is one of the values above.  `pid' names the tracee (ignored by
 * PTRACE_TRACEME).  What `addr' and `data' mean depends on the request:
 *
 *   PEEK*        `addr' is the address to read; `data' unused.  The word read
 *                is the return value.
 *   POKE*        `addr' is the address, `data' the word to write.
 *   GETREGS      `addr' is a `struct ptrace_regs *'; `data' unused.
 *   SETREGS      likewise.
 *   GETFPREGS    `addr' is a `struct ptrace_fpregs *'; `data' unused.
 *   SETFPREGS    likewise.
 *   CONT         `data' is a signal to deliver on resume, or 0; `addr' unused.
 *   SINGLESTEP   likewise.  SYSCALL likewise.
 *   DETACH       likewise.
 *   SETOPTIONS   `data' is a mask of PTRACE_O_*.
 *   GETEVENTMSG  `data' is an `unsigned long *'.
 *
 * The register requests taking their buffer in `addr' rather than `data', and
 * `data' being an integer rather than a pointer, are both deliberate: that is
 * the shape the portable ptrace code in debuggers is written against, and
 * following it is what lets that code be reused here rather than
 * reimplemented.  A request wanting a pointer in `data' (GETEVENTMSG) takes it
 * cast to long, which is what such interfaces have always required.
 *
 * Returns 0 on success for most requests; the PEEK requests return the word
 * they read.  On failure returns -1 with errno set -- EPERM if the caller may
 * not trace the target, ESRCH if there is no such process or it is not
 * stopped, EFAULT for an address the tracee does not have mapped, EINVAL for
 * a malformed request.
 *
 * Because a PEEK can legitimately read -1, callers that need to tell that
 * apart from an error must clear errno first and check it afterwards. */
long ptrace(int request, pid_t pid, void *addr, long data);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PTRACE_H */
