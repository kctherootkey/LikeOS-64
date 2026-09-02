// LikeOS-64 Signal Definitions
#ifndef _KERNEL_SIGNAL_H_
#define _KERNEL_SIGNAL_H_

#include <kernel/ke/fpu.h>
#include <kernel/uapi/types.h>

// Forward declarations
struct task;
struct interrupt_frame;

// Signal numbers (the conventional numbering)
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGIOT 6 // Alias for SIGABRT
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPOLL SIGIO
#define SIGPWR 30
#define SIGSYS 31
#define SIGUNUSED 31

#define SIGRTMIN 32
#define SIGRTMAX 64

#define NSIG 65 // Total number of signals (0-64)
#define _NSIG NSIG

// Signal set operations
#define _SIGSET_NWORDS 1

typedef struct {
	uint64_t sig[_SIGSET_NWORDS];
} kernel_sigset_t;

// Signal handler types
typedef void (*sighandler_t)(int);
typedef void (*sigaction_handler_t)(int, void *,
				    void *); // siginfo_t*, ucontext_t*

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t) - 1)

// sigaction flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER 0x04000000

// sigprocmask how values
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// signalfd flags
#define SFD_CLOEXEC 02000000
#define SFD_NONBLOCK 00004000

// siginfo_t structure
typedef struct siginfo {
	int si_signo; // Signal number
	int si_errno; // errno value
	int si_code; // Signal code
	int _pad0;
	union {
		int _pad[28]; // Padding for extensibility

		// kill(), sigsend(), raise()
		struct {
			int32_t si_pid; // Sending process ID
			uint32_t si_uid; // Real user ID of sender
		} _kill;

		// POSIX.1b timers
		struct {
			int32_t si_tid; // Timer ID
			int32_t si_overrun; // Overrun count
			int32_t si_int; // Signal value (int)
			void *si_ptr; // Signal value (pointer)
		} _timer;

		// SIGCHLD
		struct {
			int32_t si_pid; // Child PID
			uint32_t si_uid; // Real user ID of sender
			int32_t si_status; // Exit value or signal
			int64_t si_utime; // User time consumed
			int64_t si_stime; // System time consumed
		} _sigchld;

		// SIGILL, SIGFPE, SIGSEGV, SIGBUS
		struct {
			void *si_addr; // Faulting instruction/memory address
			int16_t si_addr_lsb;
			int16_t _pad1;
		} _sigfault;

		// SIGPOLL
		struct {
			int64_t si_band; // Band event
			int32_t si_fd; // File descriptor
		} _sigpoll;
	} _sifields;
} siginfo_t;

// Accessor macros
/* If this fires, siginfo_t here and in user/lib/libc/include/signal.h have
 * drifted apart -- and the kernel copies this struct verbatim into a signal
 * frame and out through PTRACE_GETSIGINFO, so whichever is larger writes over
 * the other one's memory. */
_Static_assert(sizeof(siginfo_t) == 128,
	       "siginfo_t must match user/lib/libc/include/signal.h");

#define si_pid _sifields._kill.si_pid
#define si_uid _sifields._kill.si_uid
#define si_timerid _sifields._timer.si_tid
#define si_overrun _sifields._timer.si_overrun
#define si_status _sifields._sigchld.si_status
#define si_utime _sifields._sigchld.si_utime
#define si_stime _sifields._sigchld.si_stime
#define si_addr _sifields._sigfault.si_addr
#define si_band _sifields._sigpoll.si_band
#define si_fd _sifields._sigpoll.si_fd
#define si_int _sifields._timer.si_int
#define si_ptr _sifields._timer.si_ptr

// si_code values
#define SI_USER 0 // kill()
#define SI_KERNEL 128 // Kernel
#define SI_QUEUE -1 // sigqueue()
#define SI_TIMER -2 // Timer
#define SI_MESGQ -3 // Message queue
#define SI_ASYNCIO -4 // Async I/O
#define SI_SIGIO -5 // Signal I/O
#define SI_TKILL -6 // tkill

/* si_code values for the synchronous fault signals.
 *
 * A debugger reads these: they are the difference between "it crashed" and
 * "it wrote to an address that is not mapped".  gdb surfaces them through
 * $_siginfo and in the message it prints when the inferior stops. */
#define SEGV_MAPERR 1 /* address not mapped */
#define SEGV_ACCERR 2 /* mapped, but the access was not permitted */
#define ILL_ILLOPC 1 /* illegal opcode */
#define ILL_ILLOPN 2 /* illegal operand */
#define ILL_PRVOPC 5 /* privileged opcode */
#define FPE_INTDIV 1 /* integer divide by zero */
#define FPE_INTOVF 2 /* integer overflow */
#define FPE_FLTDIV 3 /* floating-point divide by zero */
#define FPE_FLTINV 7 /* invalid floating-point operation */
#define BUS_ADRALN 1 /* invalid address alignment */
#define BUS_ADRERR 2 /* non-existent physical address */
#define TRAP_BRKPT 1 /* process breakpoint */
#define TRAP_TRACE 2 /* process trace trap */
#define TRAP_HWBKPT 4 /* hardware breakpoint or watchpoint */

// SIGCHLD si_code
#define CLD_EXITED 1 // Child exited
#define CLD_KILLED 2 // Child killed
#define CLD_DUMPED 3 // Child dumped core
#define CLD_TRAPPED 4 // Traced child trapped
#define CLD_STOPPED 5 // Child stopped
#define CLD_CONTINUED 6 // Child continued

// sigaction structure (kernel version)
struct k_sigaction {
	union {
		sighandler_t sa_handler;
		sigaction_handler_t sa_sigaction;
	} _u;
	uint64_t sa_flags;
	void (*sa_restorer)(void);
	kernel_sigset_t sa_mask;
};

#define sa_handler _u.sa_handler
#define sa_sigaction _u.sa_sigaction

// Stack for signal handlers
typedef struct sigaltstack {
	void *ss_sp; // Stack pointer
	int ss_flags; // Flags
	size_t ss_size; // Stack size
} stack_t;

// sigaltstack flags
#define SS_ONSTACK 1
#define SS_DISABLE 2
/* The frame a handler is entered with carries the whole extended register
 * file (up to FPU_STATE_MAX bytes), so the smallest usable alternate stack
 * is a good deal larger than the historical 2 KB. */
#define MINSIGSTKSZ 12288
#define SIGSTKSZ 65536

// Interval timer types
#define ITIMER_REAL 0 // Real time (SIGALRM)
#define ITIMER_VIRTUAL 1 // Virtual time (SIGVTALRM)
#define ITIMER_PROF 2 // Profiling time (SIGPROF)

struct k_timeval {
	int64_t tv_sec; // Seconds
	int64_t tv_usec; // Microseconds
};

struct k_itimerval {
	struct k_timeval it_interval; // Interval for periodic timer
	struct k_timeval it_value; // Time until next expiration
};

// POSIX timers
typedef int32_t clockid_t;
typedef int32_t ktimer_t;

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME 2
#define CLOCK_THREAD_CPUTIME 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
/* clock_nanosleep(): the request is an absolute time on the clock. */
#define TIMER_ABSTIME 1

struct k_timespec {
	int64_t tv_sec; // Seconds
	int64_t tv_nsec; // Nanoseconds
};

struct k_itimerspec {
	struct k_timespec it_interval;
	struct k_timespec it_value;
};

// sigevent for timer creation
#define SIGEV_SIGNAL 0
#define SIGEV_NONE 1
#define SIGEV_THREAD 2
#define SIGEV_THREAD_ID 4

union k_sigval {
	int sival_int;
	void *sival_ptr;
};

struct k_sigevent {
	union k_sigval sigev_value;
	int sigev_signo;
	int sigev_notify;
	int sigev_tid; // Thread ID for SIGEV_THREAD_ID
	int _pad;
};

// signalfd_siginfo structure
struct signalfd_siginfo {
	uint32_t ssi_signo; // Signal number
	int32_t ssi_errno; // Error number
	int32_t ssi_code; // Signal code
	uint32_t ssi_pid; // Sender PID
	uint32_t ssi_uid; // Sender UID
	int32_t ssi_fd; // File descriptor
	uint32_t ssi_tid; // Timer ID
	uint32_t ssi_band; // Band event
	uint32_t ssi_overrun; // Overrun count
	uint32_t ssi_trapno; // Trap number
	int32_t ssi_status; // Exit status
	int32_t ssi_int; // sigqueue int
	uint64_t ssi_ptr; // sigqueue pointer
	uint64_t ssi_utime; // User CPU time
	uint64_t ssi_stime; // System CPU time
	uint64_t ssi_addr; // Fault address
	uint16_t ssi_addr_lsb; // LSB of address
	uint8_t _pad[46]; // Padding to 128 bytes
};

// Signal set operations (inline for kernel use)
static inline void sigemptyset_k(kernel_sigset_t *set)
{
	set->sig[0] = 0;
}

static inline void sigfillset_k(kernel_sigset_t *set)
{
	set->sig[0] = ~0ULL;
}

static inline void sigaddset_k(kernel_sigset_t *set, int sig)
{
	if (sig > 0 && sig < NSIG) {
		set->sig[0] |= (1ULL << (sig - 1));
	}
}

static inline void sigdelset_k(kernel_sigset_t *set, int sig)
{
	if (sig > 0 && sig < NSIG) {
		set->sig[0] &= ~(1ULL << (sig - 1));
	}
}

static inline int sigismember_k(const kernel_sigset_t *set, int sig)
{
	if (sig <= 0 || sig >= NSIG)
		return 0;
	return (set->sig[0] & (1ULL << (sig - 1))) ? 1 : 0;
}

static inline void sigorset_k(kernel_sigset_t *dest, const kernel_sigset_t *a,
			      const kernel_sigset_t *b)
{
	dest->sig[0] = a->sig[0] | b->sig[0];
}

static inline void sigandset_k(kernel_sigset_t *dest, const kernel_sigset_t *a,
			       const kernel_sigset_t *b)
{
	dest->sig[0] = a->sig[0] & b->sig[0];
}

static inline void signandset_k(kernel_sigset_t *dest, const kernel_sigset_t *a,
				const kernel_sigset_t *b)
{
	dest->sig[0] = a->sig[0] & ~b->sig[0];
}

static inline int sigisemptyset_k(const kernel_sigset_t *set)
{
	return set->sig[0] == 0;
}

// Unmaskable signals
#define sig_kernel_only(sig) ((sig) == SIGKILL || (sig) == SIGSTOP)

/* SIGKILL and SIGSTOP are unblockable by contract: a task must always notice
 * them whatever mask it installs, or it becomes unkillable.  Run every mask
 * that originates in (or can be influenced by) userspace through here before
 * installing it — sigprocmask, sigsuspend, sigaction's sa_mask, the mask
 * applied while a handler runs, and the mask sigreturn restores from the
 * user's own stack. */
static inline void sig_strip_unblockable(kernel_sigset_t *set)
{
	sigdelset_k(set, SIGKILL);
	sigdelset_k(set, SIGSTOP);
}

// Default action types
#define SIG_DFL_TERM 0 // Terminate
#define SIG_DFL_IGN 1 // Ignore
#define SIG_DFL_CORE 2 // Core dump + terminate
#define SIG_DFL_STOP 3 // Stop process
#define SIG_DFL_CONT 4 // Continue if stopped

// Get default signal action
static inline int sig_default_action(int sig)
{
	switch (sig) {
	case SIGCHLD:
	case SIGURG:
	case SIGWINCH:
		return SIG_DFL_IGN;
	case SIGSTOP:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
		return SIG_DFL_STOP;
	case SIGCONT:
		return SIG_DFL_CONT;
	case SIGQUIT:
	case SIGILL:
	case SIGTRAP:
	case SIGABRT:
	case SIGBUS:
	case SIGFPE:
	case SIGSEGV:
	case SIGXCPU:
	case SIGXFSZ:
	case SIGSYS:
		return SIG_DFL_CORE;
	default:
		return SIG_DFL_TERM;
	}
}

// Maximum pending signals per task
#define MAX_PENDING_SIGNALS 32

/* ---- Machine context handed to a signal handler --------------------------
 *
 * The third argument of an SA_SIGINFO handler points at this.  The register
 * order in gregs[] is the conventional x86-64 one (the REG_* indices below),
 * which is what portable code -- debuggers, language runtimes that resume a
 * faulting instruction by editing REG_RIP, garbage collectors that scan a
 * suspended thread's registers -- has been written against.
 *
 * The frame is the ONLY copy of the interrupted registers: sigreturn reads
 * them back from here, so a handler that writes to gregs[] changes where and
 * with what the interrupted code resumes.  That is the documented contract
 * of ucontext, not an accident of layout.
 *
 * This layout is mirrored byte for byte by <sys/ucontext.h> in the libc. */
enum {
	REG_R8 = 0, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14,
	REG_R15, REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX,
	REG_RCX, REG_RSP, REG_RIP, REG_EFL, REG_CSGSFS, REG_ERR, REG_TRAPNO,
	REG_OLDMASK, REG_CR2, NGREG
};

typedef struct mcontext {
	uint64_t gregs[NGREG];
	/* Points at the extended register image in the frame (FXSAVE layout
	 * for the first 512 bytes, XSAVE header and components after it). */
	void *fpregs;
	uint64_t __reserved[8];
} mcontext_t;

typedef struct ucontext {
	uint64_t uc_flags;
	struct ucontext *uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	kernel_sigset_t uc_sigmask;
	uint8_t __reserved[64];
} ucontext_t;

/* Bytes reserved in every frame for the extended register image.  Fixed
 * rather than g_fpu_state_size so the frame has one shape; the image is
 * 64-byte aligned within the frame, as XSAVE demands. */
#define SIGFRAME_FPU_SIZE FPU_STATE_MAX

/* Signal frame -- what the kernel writes on the user stack to enter a
 * handler, and what sys_rt_sigreturn reads back.  Laid out on the stack as
 *
 *     frame_addr:                 signal_frame_t (this struct)
 *     align_up(end, 64):          SIGFRAME_FPU_SIZE bytes of register image
 *
 * The handler is entered with RSP == frame_addr, so `pretcode' is what its
 * `ret' pops.  Everything the interrupted context needs lives in `uc'. */
typedef struct signal_frame {
	uint64_t pretcode; // return address: sa_restorer or &retcode
	ucontext_t uc; // registers, mask, altstack state
	int sig;
	int __pad;
	siginfo_t info;
	// Sigreturn trampoline (used when sa_restorer is not set)
	uint8_t retcode[16];
} signal_frame_t;

/* Where the register image sits for a frame at `frame_addr'. */
static inline uint64_t sigframe_fpu_addr(uint64_t frame_addr)
{
	return (frame_addr + sizeof(signal_frame_t) + 63) & ~63ULL;
}

// Pending signal queue entry
typedef struct pending_signal {
	int sig;
	siginfo_t info;
	struct pending_signal *next;
} pending_signal_t;

// Task signal state (embedded in task_t)
typedef struct task_signal_state {
	struct k_sigaction action[NSIG]; // Signal handlers
	kernel_sigset_t blocked; // Blocked signals mask
	kernel_sigset_t pending; // Pending signals bitmask
	pending_signal_t *pending_queue; // Queue for siginfo
	/* Woken whenever a signal is queued for this task: signalfd readers
	 * and pollers sleep here (the signals they want are blocked, so the
	 * ordinary "actionable signal" wake does not apply to them).
	 * Allocated on first signalfd use (signalfd_task_wq), freed with the
	 * task; NULL for the many tasks that never use a signalfd. */
	struct wait_queue_head *sigfd_wq;
	kernel_sigset_t saved_mask; // Saved mask for sigsuspend
	int in_sigsuspend; // Currently in sigsuspend
	stack_t altstack; // Alternate signal stack
	struct k_itimerval itimer_real; // ITIMER_REAL
	struct k_itimerval itimer_virtual; // ITIMER_VIRTUAL
	struct k_itimerval itimer_prof; // ITIMER_PROF
	uint64_t alarm_ticks; // alarm() expiration tick
	uint64_t
		/* How many signal frames this task currently has on its user
		 * stack.  A COUNT, not an address: handlers nest, and one slot
		 * holding "the" frame address could only ever describe the
		 * innermost one.  The outer handler's sigreturn then found the
		 * slot already cleared by the inner one and failed, and since
		 * the sigreturn trampoline has nowhere to return to it fell
		 * through to its own `hlt' -- a general protection fault in
		 * user code, reported as SIGSEGV at an address that is not the
		 * program's fault.  ctwm and pcmanfm both died that way on a
		 * shutdown signal followed by a restart signal.
		 *
		 * The frame's ADDRESS comes from the user stack pointer at the
		 * sigreturn call, which is where the frame demonstrably is; see
		 * signal_restore_frame().  This count is only the guard that
		 * says a sigreturn is happening inside a handler at all. */
		signal_frame_depth;
} task_signal_state_t;

// Kernel POSIX timer
#define MAX_POSIX_TIMERS 32

typedef struct kernel_timer {
	int in_use;
	ktimer_t timerid;
	clockid_t clockid;
	struct k_sigevent sevp;
	struct k_itimerspec spec;
	uint64_t next_tick; // Next expiration in ticks
	uint64_t interval_ticks; // Interval in ticks
	int overrun;
	int owner_pid;
} kernel_timer_t;

// Forward declaration
struct task;

// DEPRECATED: These global externs are no longer used.
// Signal delivery and syscall return now use per-CPU storage in percpu_t.
// Kept for backward compatibility but should not be referenced.
extern uint64_t syscall_saved_user_rip;
extern uint64_t syscall_saved_user_rsp;
extern uint64_t syscall_saved_user_rflags;
extern uint64_t syscall_saved_user_rbp;
extern uint64_t syscall_saved_user_rbx;
extern uint64_t syscall_saved_user_r12;
extern uint64_t syscall_saved_user_r13;
extern uint64_t syscall_saved_user_r14;
extern uint64_t syscall_saved_user_r15;
extern uint64_t syscall_saved_user_rax;

// Signal API for kernel use
void signal_init_task(struct task *task);
/* The task's signalfd wait queue, allocated on first use (NULL when the
 * allocation fails).  kernel/fs/signalfd.c */
struct wait_queue_head;
struct wait_queue_head *signalfd_task_wq(struct task *task);
void signal_reset_on_exec(struct task *task);
void signal_fork_copy(struct task *child, struct task *parent);
/* The same for a clone that SHARES its creator's handlers (CLONE_SIGHAND,
 * which CLONE_THREAD implies): the dispositions and the blocked mask stay,
 * everything per-task is reset.  Must be called for every such clone -- the
 * task_t was produced by a wholesale copy, so skipping it leaves two tasks
 * owning one signalfd wait queue and one pending-siginfo list. */
void signal_thread_copy(struct task *child, struct task *parent);
void signal_cleanup_task(struct task *task);
int signal_send(struct task *task, int sig, siginfo_t *info);
int signal_send_group(int pgid, int sig, siginfo_t *info);

/* Record a stop/continue on `task` for waitpid(WUNTRACED/WCONTINUED) and
 * notify its parent (SIGCHLD + waitpid wakeup).  stopped=1 for a stop by
 * signal `signum`, 0 for a continue. */
void signal_notify_jobctl(struct task *task, int signum, int stopped);
/* May the calling task send `sig` to `target`?  POSIX rule: the privileged
 * caller may signal anyone; otherwise the sender's real or effective uid must
 * match the target's real or saved-set uid (SIGCONT is also allowed within the
 * same session).  Returns 0 if permitted, or -EPERM. */
/* RFLAGS the kernel is willing to give user mode.
 *
 * Everything that returns to user mode goes through this.  Two reasons:
 *
 * IF is FORCED ON.  A user thread resumed with interrupts disabled cannot be
 * preempted and takes every fault with IRQs off -- so a page fault on a
 * demand-paged text page then read from ext4 with interrupts disabled, which
 * is the "might_sleep() called with IRQs disabled" storm, and the
 * "USER RIP ... with interrupts disabled ... the saved RFLAGS is wrong"
 * warning that precedes it.  The saved RFLAGS was not wrong; it was obeyed.
 *
 * And the source of it is reachable from user code: sigreturn(2) restores
 * RFLAGS from the signal frame on the USER stack, which the process can edit.
 * Unfiltered, that let any program clear its own IF -- pinning a CPU with
 * interrupts off -- or set IOPL and NT.  Only the flags a program may set for
 * itself survive; the rest come from here.
 */
#define USER_RFLAGS_KEEP                                                  	(0x1UL /*CF*/ | 0x2UL /*reserved, must be 1*/ | 0x4UL /*PF*/ |     	 0x10UL /*AF*/ | 0x40UL /*ZF*/ | 0x80UL /*SF*/ | 0x100UL /*TF*/ | 	 0x400UL /*DF*/ | 0x800UL /*OF*/ | 0x200000UL /*ID*/)

static inline uint64_t user_rflags_sanitize(uint64_t f)
{
	return (f & USER_RFLAGS_KEEP) | 0x202UL; /* bit 1 + IF */
}

int signal_permission(struct task *target, int sig);
int signal_pending(struct task *task);
int signal_should_restart(struct task *task);
int signal_dequeue(struct task *task, kernel_sigset_t *mask, siginfo_t *info);
/* ...and the signalfd form: `want' is the set to TAKE, not the set to skip. */
int signal_dequeue_wanted(struct task *task, const kernel_sigset_t *want,
			  siginfo_t *info);
/* Queue a synchronous fault signal (SIGSEGV/SIGILL/SIGBUS/SIGFPE) with the
 * si_code and si_addr a debugger needs, forcing the disposition back to default
 * if the program blocked or ignored it -- a fault cannot be declined, since
 * returning to the instruction that caused it just causes it again.
 *
 * Decides nothing else: no stop, no kill, no report.  The return-to-user path
 * acts on it, which is the only context where a tracer can be consulted. */
void signal_force_fault(struct task *task, int sig, int code, uint64_t addr);

void signal_deliver(struct task *task);
void signal_deliver_irq(struct task *task, struct interrupt_frame *frame);
void signal_check_timers(struct task *task, uint64_t current_tick);

// Signal frame setup/restore for syscall handling
int signal_setup_frame(struct task *task, int sig, siginfo_t *info,
		       struct k_sigaction *act);
int signal_setup_frame_irq(struct task *task, int sig, siginfo_t *info,
			   struct k_sigaction *act,
			   struct interrupt_frame *frame);
int signal_restore_frame(struct task *task);

/* Shared with the syscall layer split out of ke/syscall.c. */
struct task;
int64_t signal_target_check(struct task *target, int sig);

#endif // _KERNEL_SIGNAL_H_
